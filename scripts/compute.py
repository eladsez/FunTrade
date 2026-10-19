import numpy as np
import pandas as pd
import yfinance as yf
import statsmodels.api as sm
from statsmodels.tsa.stattools import adfuller
from statsmodels.tsa.vector_ar.vecm import coint_johansen
from itertools import combinations, product
from datetime import datetime, timedelta
import json
import warnings
import os
from joblib import Parallel, delayed # For parallel processing

warnings.filterwarnings("ignore")


def fetch_and_prepare_data(tickers, years=5, split_ratio=0.8):
    # 1. Date Setup
    end_date = datetime.now().strftime('%Y-%m-%d')
    start_date = (datetime.now() - timedelta(days=years*365)).strftime('%Y-%m-%d')

    print(f"[*] Downloading {len(tickers)} tickers and SPY benchmark...")

    # 2. Download Stocks & Benchmark
    data = yf.download(tickers, start=start_date, end=end_date, threads=True, auto_adjust=True)['Close']
    spy = yf.download("SPY", start=start_date, end=end_date, threads=True, auto_adjust=True)['Close']
    
    # 3. Clean the Ticker Data
    # Drop columns (tickers) that have more than 10% missing data
    threshold = len(data) * 0.9
    data = data.dropna(thresh=threshold, axis=1)
    
    # Drop remaining rows with NaNs to create our master trading calendar
    clean_data = data.dropna()
    print(f"[*] Proceeding with {len(clean_data.columns)} valid tickers.")
    
    # If the clean dataset is too small, abort early
    if len(clean_data) < 50:
        return None

    # 4. Process Benchmark (SPY)
    spy_returns = np.log(spy).diff()

    # 5. Timezone Strip & Alignment
    # Strip timezones from both to ensure the dates "talk" to each other
    clean_data.index = clean_data.index.tz_localize(None)
    spy_returns.index = spy_returns.index.tz_localize(None)

    # Force SPY to match the clean stock calendar perfectly
    spy_returns = spy_returns.reindex(clean_data.index, method='ffill').fillna(0)

    # 6. Split Data
    split_idx = int(len(clean_data) * split_ratio)
    
    df_train = clean_data.iloc[:split_idx]
    df_test = clean_data.iloc[split_idx:]
    bench_train = spy_returns.iloc[:split_idx]

    # 7. Extract Dates for Config Export
    t_start = df_train.index[0].strftime('%Y-%m-%d')
    t_end = df_train.index[-1].strftime('%Y-%m-%d')
    oos_start = df_test.index[0].strftime('%Y-%m-%d')
    oos_end = df_test.index[-1].strftime('%Y-%m-%d')

    return {
        "train": df_train,
        "test": df_test,
        "benchmark_train": bench_train,
        "dates": (t_start, t_end, oos_start, oos_end)
    }

def calculate_half_life(spread):
    spread_lag = spread.shift(1).dropna()
    spread_diff = spread.diff().dropna()
    X = sm.add_constant(spread_lag)
    model = sm.OLS(spread_diff, X).fit()
    lam = model.params.iloc[1]
    return -np.log(2) / lam if lam < 0 else np.inf

def calculate_hurst(ts, max_lag=20):
    """
    Calculates the Hurst Exponent using variance of lagged differences.
    Returns a value between 0 and 1.
    < 0.5 means mean-reverting.
    """
    # Ensure it's a clean numpy array
    ts = np.asarray(ts)
    ts = ts[~np.isnan(ts)]
    
    # We need enough data to calculate lags
    if len(ts) < max_lag * 2:
        return 0.5 
        
    lags = range(2, max_lag)
    
    # Calculate the variance of the lagged differences
    tau = [np.var(np.subtract(ts[lag:], ts[:-lag])) for lag in lags]
    
    # Linear fit: log(variance) vs log(lags)
    # The slope is 2H, so H = slope / 2.0
    poly = np.polyfit(np.log(lags), np.log(tau), 1)
    return poly[0] / 2.0

def calculate_execution_shares(current_prices, weights, direction, max_capital=5000.0, max_error=0.025):
    """
    Calculates exact integer share counts for ALL legs.
    """
    total_abs_weight = sum(abs(w) for w in weights.values())
    ratios = {t: abs(w) / total_abs_weight for t, w in weights.items()}
    
    # 1. Base capital required to buy at least 1 share of the most "restrictive" leg
    capital_reqs = [current_prices[t] / ratios[t] for t in weights.keys()]
    base_capital = max(capital_reqs)
    
    if base_capital > max_capital:
        return None 
        
    # 2. Dynamic Scaling
    multiplier = int(max_capital // base_capital)
    if multiplier == 0:
        return None
        
    target_cap = base_capital * multiplier
    shares = {}
    total_actual_cap = 0.0
    
    # 3. Lock in Shares for all legs
    for t in weights.keys():
        ideal_cap = target_cap * ratios[t]
        shares[t] = round(ideal_cap / current_prices[t]) 
        
        if shares[t] == 0:
            return None
            
        total_actual_cap += (shares[t] * current_prices[t])
        
    # 4. Final Tracking Error Gatekeeper
    for t in weights.keys():
        actual_ratio = (shares[t] * current_prices[t]) / total_actual_cap
        if abs(actual_ratio - ratios[t]) / ratios[t] > max_error:
            return None
            
    return shares


def check_tradeability_dynamic(weights, max_prices, max_capital=5000.0, max_error=0.025):
    """
    Stress-tests the basket against the worst-case (highest) prices.
    If it survives the most expensive day, it is tradeable.
    """
    for direction in [1, -1]:
        shares = calculate_execution_shares(max_prices, weights, direction, max_capital, max_error)
        if shares is None:
            return False
    return True

def simulate_event_loop(z_spread, prices, weights, entry_z, exit_z, cost_per_trade=0.002, max_capital=5000.0,
                        size_with_prev_close=True):
    """
    Chronological state machine simulating actual execution logic.
    Returns the final Sharpe ratio and total number of trades.

    Orders fill at today's close (MOC). Entries are sized with YESTERDAY's close by default,
    because that is the last price known when the order is sent; sizing with today's close
    (size_with_prev_close=False, the original behaviour) uses a price nobody has yet.
    scripts/parity_check.py verifies the C++ bot trades exactly this.
    """
    current_position = 0 # 1 for Long Spread, -1 for Short Spread, 0 for Flat
    current_shares = {}
    
    daily_pnl = []
    trade_count = 0
    
    # Align prices to the Z-score timeline
    aligned_prices = prices.loc[z_spread.index]
    
    for i in range(1, len(z_spread)):
        today_z = z_spread.iloc[i-1] # Yesterday's close determines today's action
        today_prices = aligned_prices.iloc[i].to_dict()
        yesterday_prices = aligned_prices.iloc[i-1].to_dict()
        sizing_prices = yesterday_prices if size_with_prev_close else today_prices
        
        day_pnl = 0.0
        
        # 1. Mark-to-Market (Calculate PnL if holding a position)
        if current_position != 0:
            for t, shares in current_shares.items():
                price_diff = today_prices[t] - yesterday_prices[t]
                # If we are Short the spread, weights with negative signs were bought long, etc.
                # The execution logic handles the sign abstraction, we just calculate absolute holdings value change.
                # Wait, calculate_execution_shares returns absolute share counts. We must apply the direction.
                leg_direction = np.sign(weights[t]) * current_position
                day_pnl += shares * price_diff * leg_direction

        # 2. Check Exits
        if current_position == 1 and today_z >= -exit_z:
            current_position = 0
            current_shares = {}
            day_pnl -= (max_capital * cost_per_trade) # Abstracted friction cost
            trade_count += 1
            
        elif current_position == -1 and today_z <= exit_z:
            current_position = 0
            current_shares = {}
            day_pnl -= (max_capital * cost_per_trade)
            trade_count += 1
            
        # 3. Check Entries
        elif current_position == 0:
            if today_z < -entry_z:
                new_shares = calculate_execution_shares(sizing_prices, weights, direction=1, max_capital=max_capital)
                if new_shares: # Only enter if the tracking error allows it today
                    current_position = 1
                    current_shares = new_shares
                    day_pnl -= (max_capital * cost_per_trade)
                    trade_count += 1
                    
            elif today_z > entry_z:
                new_shares = calculate_execution_shares(sizing_prices, weights, direction=-1, max_capital=max_capital)
                if new_shares:
                    current_position = -1
                    current_shares = new_shares
                    day_pnl -= (max_capital * cost_per_trade)
                    trade_count += 1
                    
        daily_pnl.append(day_pnl)
        
    pnl_series = pd.Series(daily_pnl)
    # Convert dollar PnL to percentage returns against max_capital for standard metrics
    pct_returns = pnl_series / max_capital 
    
    mean_pnl = pct_returns.mean()
    std_pnl = pct_returns.std()
    sharpe = np.sqrt(252) * (mean_pnl / std_pnl) if std_pnl > 0 else 0.0
    
    return sharpe, trade_count, pct_returns


def find_best_params(spread, window_size, prices, weights, cost_per_trade=0.002, max_capital=5000.0):
    entry_range = np.arange(1.5, 3.25, 0.25)
    exit_range = np.arange(-0.25, 0.75, 0.25)
    
    rolling_mu = spread.rolling(window=window_size).mean()
    rolling_std = spread.rolling(window=window_size).std()
    z_spread = ((spread - rolling_mu) / rolling_std).dropna()
    
    results = []

    for entry_z, exit_z in product(entry_range, exit_range):
        if exit_z >= entry_z: 
            continue 
            
        sharpe, _, _ = simulate_event_loop(
            z_spread=z_spread, 
            prices=prices, 
            weights=weights, 
            entry_z=entry_z, 
            exit_z=exit_z, 
            cost_per_trade=cost_per_trade,
            max_capital=max_capital
        )
        
        results.append({
            'entry': entry_z, 
            'exit': exit_z, 
            'sharpe': sharpe
        })

    if not results: 
        return 2.0, 0.0, 0.0
    
    best = max(results, key=lambda x: x['sharpe'])
    return round(float(best['entry']), 2), round(float(best['exit']), 2), round(float(best['sharpe']), 3)


def test_basket(df, combo, benchmark_returns, cost_per_trade=0.002):
    subset_df = df[list(combo)]
    
    try:
        # Convert raw prices to log prices to ensure dollar-neutrality
        # and proportional capital allocation.
        log_df = np.log(subset_df)
        
        # Individual Stationarity Check (The I(1) Rule)
        # Cointegration implies a linear combination of NON-stationary 
        # time series is stationary. If an individual leg is already 
        # stationary, the Johansen test results are invalid.
        for ticker in combo:
            ind_p_val = adfuller(log_df[ticker])[1]
            # If p-value < 0.05, the individual stock is stationary. Reject basket.
            if ind_p_val < 0.05:
                return None
        
        # Run Johansen on the log prices
        joh_res = coint_johansen(log_df.values, 0, 1)
        # Newer statsmodels returns complex eigenvectors (imaginary part is 0); round() fails on complex.
        raw_weights = np.real(joh_res.evec[:, 0])

        # Normalize weights against the largest absolute weight to prevent explosion
        max_idx = np.argmax(np.abs(raw_weights))
        anchor_weight = raw_weights[max_idx]
        weights = {combo[i]: round(raw_weights[i] / anchor_weight, 4) for i in range(len(combo))}

        # STRICT GATEKEEPER: Drop baskets where a leg becomes irrelevant
        if any(w == 0.0 for w in weights.values()):
            return None

        # Weight Explosion Sanity Check
        # Prevent mathematically sound but practically untradeable allocations.
        # Calculate the absolute total weight (representing total capital required)
        total_abs_weight = sum(abs(w) for w in weights.values())
        # Define the maximum allowed concentration (e.g., 60% of total capital)
        max_concentration = 0.60
        for ticker, w in weights.items():
            concentration = abs(w) / total_abs_weight
            if concentration > max_concentration:
                # Reject if any single leg dominates the portfolio risk
                return None
        
        # Spread is now a log-spread
        spread = sum(log_df[t] * weights[t] for t in combo)
        
        # --- Market Neutrality (Beta) Check ---
        spread_returns = spread.diff().dropna()
        
        # Align indices to ensure we only compare matching days
        aligned_data = pd.concat([spread_returns, benchmark_returns], axis=1, join='inner').dropna()
        aligned_data.columns = ['Spread', 'Market']
        
        # Calculate Beta: Covariance(Spread, Market) / Variance(Market)
        cov = aligned_data.cov().iloc[0, 1]
        var = aligned_data['Market'].var()
        beta = cov / var
        
        # STRICT FILTER: Disqualify if absolute beta is greater than 0.15
        if abs(beta) > 0.15:
            return None
        # -------------------------------------------
        
        p_val = adfuller(spread)[1]
        hl = calculate_half_life(spread)
        hurst_exp = calculate_hurst(spread)

        # STRICT GATEKEEPER:
        # 1. ADF P-Value must be < 0.05 (Stationary)
        # 2. Half-life must be between 2 and 60 days
        # 3. Hurst must be < 0.45 (Strong Mean Reversion)
        if p_val > 0.05 or not (2 <= hl <= 60) or hurst_exp >= 0.45: 
            return None

        # --- TRADEABILITY STRESS TEST ---
        # Extract the absolute maximum price for each leg during the training period.
        # We test against max prices because higher prices force smaller share counts, 
        # which violently amplifies integer rounding errors on the short side.
        # If the generated weights cannot be mapped to integers without breaking 
        # the 2.5% tracking error limit on the allocated capital, reject the basket.
        # --- Catch the dynamically calculated capitals ---
        max_prices = subset_df.max().to_dict()
        tradeable = check_tradeability_dynamic(weights, max_prices, max_capital=5000.0, max_error=0.025)
        if not tradeable:
            return None

        window_size = int(max(round(hl * 2), 20))

        # Grid search
        entry_z, exit_z, sharpe = find_best_params(
            spread=spread, 
            window_size=window_size, 
            prices=subset_df, 
            weights=weights, 
            cost_per_trade=cost_per_trade
        )
        
        # Filter out noise
        if sharpe <= 0.1: 
            return None 

        return {
            "tickers": combo, 
            "weights": weights, 
            "beta": round(beta, 3),
            "hurst": round(hurst_exp, 3),
            "p_value": p_val,
            "half_life": hl, 
            "entry_z": entry_z, 
            "exit_z": exit_z,
            "sharpe": sharpe
        }

    except Exception as e: 
        print(f"Crash on {combo}: {e}")
        return None

def validate_oos_advanced(oos_data, train_res, cost_per_trade=0.002, max_capital=5000.0):
    tickers = train_res['tickers']
    weights = train_res['weights']
    entry_z = train_res['entry_z']
    exit_z = train_res['exit_z']
    
    log_df_oos = np.log(oos_data[list(tickers)])
    oos_spread = sum(log_df_oos[t] * weights[t] for t in tickers)
    
    window_size = int(max(round(train_res['half_life'] * 2), 20))
    rolling_mu = oos_spread.rolling(window=window_size).mean()
    rolling_std = oos_spread.rolling(window=window_size).std()
    z_spread = ((oos_spread - rolling_mu) / rolling_std).dropna()
    
    if len(z_spread) < 20: 
        return {"is_robust": False}

    # Execute the C++ logic simulation
    oos_sharpe, total_trades, pct_returns = simulate_event_loop(
        z_spread=z_spread,
        prices=oos_data[list(tickers)],
        weights=weights,
        entry_z=entry_z,
        exit_z=exit_z,
        cost_per_trade=cost_per_trade,
        max_capital=max_capital
    )
    
    round_trip_trades = total_trades // 2
    if round_trip_trades < 4:
        return {"is_robust": False}
        
    # Calculate Max Drawdown based on the discrete returns
    cum_returns = pct_returns.cumsum()
    running_max = cum_returns.cummax()
    drawdowns = cum_returns - running_max
    max_drawdown = abs(drawdowns.min())
    
    oos_p_val = adfuller(oos_spread)[1]
    
    is_robust = (oos_p_val < 0.05) and (oos_sharpe > 0.5) and (max_drawdown < 0.20)

    return {
        "oos_p": oos_p_val,
        "oos_sharpe": round(oos_sharpe, 3),
        "oos_mdd": round(max_drawdown, 3),
        "trades": round_trip_trades,
        "is_robust": is_robust
    }

def export_robust_configs(robust_list, train_dates, test_dates):
    # Create directory if it doesn't exist
    folder = "basket_configs"
    if not os.path.exists(folder):
        os.makedirs(folder)
        
    for i, item in enumerate(robust_list):
        # Create a unique ID based on the tickers
        basket_id = "_".join(item['tickers']).lower()
        
        config = {
            "basket_id": basket_id,
            "tickers": item['tickers'],
            "weights": item['weights'],
            "backtest_periods": {
                "training": {"start": train_dates[0], "end": train_dates[1]},
                "testing": {"start": test_dates[0], "end": test_dates[1]}
            },
            "metrics": {
                "train_p": round(item['train_p'], 6),
                "oos_p": round(item['oos_p'], 6),
                "train_hl": round(item['train_hl'], 2),
                "oos_sharpe": item['oos_sharpe']
            },
            "trading_params": {
                "window_size": int(max(round(item['train_hl'] * 2.0), 20)),
                "entry_z": item['entry_z'],
                "exit_z": item['exit_z'],
            },
            "exported_at": datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        }
        
        file_path = os.path.join(folder, f"{basket_id}.json")
        with open(file_path, 'w') as f:
            json.dump(config, f, indent=4)

    print(f"\n[!] Successfully exported {len(robust_list)} configs to /{folder}")

def load_sector_universe(file_path):
    """
    Reads a JSON file and returns a dictionary where 
    keys are sectors and values are lists of stock symbols.
    """
    try:
        with open(file_path, 'r') as file:
            sector_dict = json.load(file)
        return sector_dict
    
    except FileNotFoundError:
        print(f"Error: The file at {file_path} was not found.")
    except json.JSONDecodeError as e:
        print(f"Error: Failed to decode JSON. {e}")
    except Exception as e:
        print(f"An unexpected error occurred: {e}")
    
    return None

def process_combo(combo, df_train, df_test, benchmark_train):
    """
    Helper function to process a single basket.
    This runs entirely independently on a separate CPU core.
    """
    # 1. Train the basket
    train_res = test_basket(df_train, combo, benchmark_train)
    
    if train_res:
        # Calculate the exact window size needed for the rolling indicators
        window_size = int(max(round(train_res['half_life'] * 2), 20))
        
        # Grab the tail of the training data to 'prime' the OOS indicators
        prime_data = df_train.iloc[-window_size:]
        
        # Stitch them together
        oos_data = pd.concat([prime_data, df_test])
        
        # 2. Validate with Out-of-Sample data using the primed dataframe
        oos_metrics = validate_oos_advanced(oos_data, train_res)
        
        if oos_metrics['is_robust']:
            # 3. CRITICAL: Map the keys correctly for the export function
            return {
                "tickers": train_res['tickers'],
                "weights": train_res['weights'],
                "train_p": train_res['p_value'],      
                "train_hl": train_res['half_life'], 
                "train_sharpe": train_res['sharpe'],
                "oos_p": oos_metrics['oos_p'],
                "oos_sharpe": oos_metrics['oos_sharpe'],
                "entry_z": train_res['entry_z'],
                "exit_z": train_res['exit_z'],
            }
            
    # Return None if it fails any stability tests
    return None

def run_over_universe(universe):
    # 1. Fetch, Clean, and Split in one go
    data = fetch_and_prepare_data(universe)
    
    # Safety check if the download failed or data was wiped out
    if data is None:
        print("[!] Critical Error: Not enough valid data to proceed.")
        return

    df_train = data['train']
    df_test = data['test']
    bench_train = data['benchmark_train']
    t_start, t_end, oos_start, oos_end = data['dates']
    
    print(f"[*] Train Period: {t_start} to {t_end}")
    print(f"[*] Test Period:  {oos_start} to {oos_end}")

    # 2. Parallel Processing
    combos = list(combinations(df_train.columns, 3))
    print(f"[*] Firing up parallel processing for {len(combos)} baskets...")

    # Pass bench_train into the process_combo function
    raw_results = Parallel(n_jobs=-1, verbose=5)(
        delayed(process_combo)(combo, df_train, df_test, bench_train) for combo in combos
    )

    # 3. Clean and Export
    results_list = [res for res in raw_results if res is not None]

    if results_list:
        export_robust_configs(results_list, (t_start, t_end), (oos_start, oos_end))
        best = min(results_list, key=lambda x: x['oos_p'])
        print(f"\n🚀 BEST BASKET: {best['tickers']} (OOS P: {best['oos_p']:.6f}) \n\n")
    else:
        print("\n[!] No survivors.")

def main():
    universes = load_sector_universe("./s&p500.json")

    for sector, symbols in universes.items():
        print(f"============ Running on {sector} sector ============")
        run_over_universe(symbols)

if __name__ == "__main__":
    main()