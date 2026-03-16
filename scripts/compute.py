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
    data = yf.download(tickers, start=start_date, end=end_date, threads=True)['Close']
    spy = yf.download("SPY", start=start_date, end=end_date, threads=True)['Close']
    
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

def check_tradeability_dynamic(weights, prices, max_capital=5000.0, max_error=0.025):
    """
    Dynamically calculates the Minimum Tradeable Capital required to guarantee 
    all short legs are integers. Scales up to the max_capital limit if possible.
    """
    total_abs_weight = sum(abs(w) for w in weights.values())
    ideal_weight_ratios = {t: abs(w) / total_abs_weight for t, w in weights.items()}
    
    # Test both directions (Long Spread & Short Spread)
    for direction in [1, -1]:
        short_legs = []
        for t, w in weights.items():
            if w * direction < 0:
                short_legs.append(t)
                
        if not short_legs:
            continue # No short legs to constrain
            
        # 1. Find the Minimum Base Capital
        # Calculate the total basket capital required to buy exactly 1 share 
        # of each short leg. 
        capital_requirements = []
        for t in short_legs:
            # Capital = Price / Ideal Ratio
            req_cap = prices[t] / ideal_weight_ratios[t]
            capital_requirements.append(req_cap)
            
        # The base_capital is dictated by the most restrictive short leg.
        # At this capital level, the most restrictive short leg gets exactly 1 share,
        # and all other short legs get >= 1 share.
        base_capital = max(capital_requirements)
        
        # 2. Hard Ceiling Check
        # If the absolute minimum capital needed to trade this basket cleanly
        # exceeds your total available budget, it's untradeable.
        if base_capital > max_capital:
            return False
            
        # 3. Dynamic Scaling
        # How many full "base_capital" blocks can we fit inside the max_capital?
        # E.g., if base_capital is $2,000 and max_capital is $5,000, multiplier = 2.
        # We deploy $4,000, ensuring short legs become exact multiples of integers.
        multiplier = int(max_capital // base_capital)
        deployed_capital = base_capital * multiplier
        
        # 4. Enforce Reality and Check Tracking Error
        actual_capital = {}
        for t in weights.keys():
            ideal_leg_capital = deployed_capital * ideal_weight_ratios[t]
            
            if t in short_legs:
                # Shorts must be integers. Because we scaled by the base_capital multiplier,
                # this division will result in a near-perfect integer.
                actual_shares = round(ideal_leg_capital / prices[t])
                
                # Zero-share fallback catch (mathematically unlikely now, but safe)
                if actual_shares == 0:
                    return False
                    
                actual_capital[t] = actual_shares * prices[t]
            else:
                # Longs utilize broker fractional shares
                actual_capital[t] = ideal_leg_capital
                
        total_proposed_cap = sum(actual_capital.values())
        if total_proposed_cap == 0:
            return False
            
        # 5. The Final Tracking Error Gatekeeper
        for t in weights.keys():
            actual_ratio = actual_capital[t] / total_proposed_cap
            ideal_ratio = ideal_weight_ratios[t]
            
            error = abs(actual_ratio - ideal_ratio) / ideal_ratio
            if error > max_error:
                return False 

        # --- Store the successful capital allocation for this direction ---
        if 'direction_caps' not in locals():
            direction_caps = {}
            
        direction_caps[direction] = {
            "min_capital": round(base_capital, 2),
            "target_capital": round(deployed_capital, 2)
        }
                
    # If it survives both directions under the max_capital ceiling, it is tradeable
    return direction_caps

def find_best_params(spread, window_size, cost_per_trade=0.002):
    """ 
    Grid Search: Entry Z vs Exit Z using ROLLING statistics.
    Fully vectorized implementation to eliminate the Python 'for' loop bottleneck,
    operating at near C-level speeds via Numpy and Pandas C-extensions.
    """
    entry_range = np.arange(1.5, 3.25, 0.25)
    exit_range = np.arange(-0.25, 0.75, 0.25)
    
    # 1. Calculate rolling statistics once (outside the loop)
    rolling_mu = spread.rolling(window=window_size).mean()
    rolling_std = spread.rolling(window=window_size).std()
    
    z_spread = ((spread - rolling_mu) / rolling_std).dropna()
    log_returns = spread.diff().loc[z_spread.index]
    
    # Convert to pure numpy arrays for maximum performance inside the grid search
    z_vals = z_spread.values
    ret_vals = log_returns.values
    
    results = []

    for entry_z, exit_z in product(entry_range, exit_range):
        if exit_z >= entry_z: 
            continue 

        # 2. Vectorized State Machine
        # We process the Short side and Long side completely independently.
        
        # --- Short Side Logic ---
        # NaN means "hold previous state"
        short_pos = np.full(len(z_vals), np.nan)
        short_pos[z_vals > entry_z] = -1.0         # Trigger Short entry
        short_pos[z_vals <= exit_z] = 0.0          # Trigger Short exit
        # Forward fill to maintain position, fill initial NaNs with 0
        short_series = pd.Series(short_pos).ffill().fillna(0.0)

        # --- Long Side Logic ---
        long_pos = np.full(len(z_vals), np.nan)
        long_pos[z_vals < -entry_z] = 1.0          # Trigger Long entry
        long_pos[z_vals >= -exit_z] = 0.0          # Trigger Long exit
        long_series = pd.Series(long_pos).ffill().fillna(0.0)

        # --- Combine Positions ---
        # Since entry thresholds are distinct, they will never overlap.
        combined_pos = short_series + long_series
        
        # 3. Prevent Look-Ahead Bias
        # Today's signal trades at tomorrow's return
        actual_pos = combined_pos.shift(1).fillna(0.0)
        
        # 4. Vectorized PnL & Cost Calculation
        trades = actual_pos.diff().abs().fillna(0.0)
        
        # Because we use log-prices, the difference is approximately the % return
        raw_pnl = actual_pos * log_returns.reset_index(drop=True)
        
        # Subtract flat percentage cost on every position change
        daily_costs = trades * cost_per_trade
        net_daily_pnl = raw_pnl - daily_costs
        
        # 5. Metrics
        mean_pnl = net_daily_pnl.mean()
        std_pnl = net_daily_pnl.std()
        
        sharpe = np.sqrt(252) * (mean_pnl / std_pnl) if std_pnl > 0 else 0.0
        
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
        raw_weights = joh_res.evec[:, 0]

        # Normalize weights so the first asset always has a weight of 1.0
        weights = {combo[i]: round(raw_weights[i]/raw_weights[0], 4) for i in range(len(combo))}

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
        trade_caps = check_tradeability_dynamic(weights, max_prices, max_capital=5000.0, max_error=0.025)
        if not trade_caps:
            return None

        window_size = int(max(round(hl * 2), 20))

        # Grid search
        entry_z, exit_z, sharpe = find_best_params(spread, window_size, cost_per_trade)
        
        # Filter out noise
        if sharpe <= 0.1: 
            return None 

        return {
            "tickers": combo, 
            "weights": weights, 
            "beta": round(beta, 3),
            "hurst": round(hurst_exp, 3),
            "trade_caps": trade_caps,
            "p_value": p_val,
            "half_life": hl, 
            "entry_z": entry_z, 
            "exit_z": exit_z,
            "sharpe": sharpe
        }

    except Exception as e: 
        return None

def validate_oos_advanced(df_oos, train_res, cost_per_trade=0.002):
    """
    Simulates OOS performance using Log Prices and counts the number of trades.
    Uses a fully vectorized state machine to match the speed of the grid search.
    """
    tickers = train_res['tickers']
    weights = train_res['weights']
    entry_z = train_res['entry_z']
    exit_z = train_res['exit_z']
    
    # 1. Reconstruct the log-spread on OOS data
    log_df_oos = np.log(df_oos[list(tickers)])
    oos_spread = sum(log_df_oos[t] * weights[t] for t in tickers)
    
    # 2. Calculate rolling Z-Score
    window_size = int(max(round(train_res['half_life'] * 2), 20))
    rolling_mu = oos_spread.rolling(window=window_size).mean()
    rolling_std = oos_spread.rolling(window=window_size).std()
    z_spread = ((oos_spread - rolling_mu) / rolling_std).dropna()
    
    if len(z_spread) < 20: 
        return {"is_robust": False}

    # 3. Vectorized State Machine
    z_vals = z_spread.values
    
    # --- Short Side Logic ---
    short_pos = np.full(len(z_vals), np.nan)
    short_pos[z_vals > entry_z] = -1.0         # Trigger Short entry
    short_pos[z_vals <= exit_z] = 0.0          # Trigger Short exit
    short_series = pd.Series(short_pos, index=z_spread.index).ffill().fillna(0.0)

    # --- Long Side Logic ---
    long_pos = np.full(len(z_vals), np.nan)
    long_pos[z_vals < -entry_z] = 1.0          # Trigger Long entry
    long_pos[z_vals >= -exit_z] = 0.0          # Trigger Long exit
    long_series = pd.Series(long_pos, index=z_spread.index).ffill().fillna(0.0)

    # --- Combine and Prevent Look-Ahead Bias ---
    combined_pos = short_series + long_series
    
    # Shift position by 1: Trade tomorrow based on today's closing signal
    actual_pos = combined_pos.shift(1).fillna(0.0)

    # 4. Count Trades and Calculate Net PnL
    # A change in position equals a trade action
    trade_actions = actual_pos.diff().abs().fillna(0.0)
    total_trade_actions = trade_actions[trade_actions > 0].count()
    round_trip_trades = int(total_trade_actions / 2)

    # STRICT FILTER: If the basket made fewer than 4 round-trips in a year, it's noise.
    if round_trip_trades < 4:
        return {"is_robust": False}

    # =====================================================================
    # 5. REALITY SIMULATION - Single-Anchor Scaling
    # =====================================================================
    cap_long = train_res['trade_caps'][1]['target_capital']
    cap_short = train_res['trade_caps'][-1]['target_capital']
    
    total_abs_weight = sum(abs(w) for w in weights.values())
    ideal_weight_ratios = {t: abs(w) / total_abs_weight for t, w in weights.items()}
    
    dollar_diffs = df_oos[list(tickers)].diff().fillna(0.0)
    
    shares_long = pd.DataFrame(index=df_oos.index)
    shares_short = pd.DataFrame(index=df_oos.index)
    
    # These will store the max tracking error across all legs for each day
    daily_error_long = pd.Series(0.0, index=df_oos.index)
    daily_error_short = pd.Series(0.0, index=df_oos.index)
    
    for direction, cap, shares_df, error_series in [(1, cap_long, shares_long, daily_error_long), 
                                                    (-1, cap_short, shares_short, daily_error_short)]:
        actual_cap_df = pd.DataFrame(index=df_oos.index)
        
        for t, w in weights.items():
            ideal_cap = cap * ideal_weight_ratios[t]
            raw_shares = ideal_cap / df_oos[t]
            
            # C++ Logic: Round shorts to nearest integer, keep longs fractional
            is_short = (w * direction < 0)
            shares_df[t] = raw_shares.round() if is_short else raw_shares
            
            # Calculate the actual capital deployed to this leg today
            actual_cap_df[t] = shares_df[t] * df_oos[t]
            
        # Calculate total actual capital deployed today
        total_actual_cap = actual_cap_df.sum(axis=1)
        
        # Calculate the tracking error for each leg and find the maximum error of the day
        for t in weights.keys():
            actual_ratio = actual_cap_df[t] / total_actual_cap
            ideal_ratio = ideal_weight_ratios[t]
            leg_error = (actual_ratio - ideal_ratio).abs() / ideal_ratio
            
            # Keep the highest error observed across any leg for this day
            error_series[:] = np.maximum(error_series, leg_error)

    # --- C++ ABORT MASK ---
    # If the daily error exceeds 2.5%, the C++ bot aborts. The position becomes 0.
    max_error_limit = 0.025
    valid_long_days = (daily_error_long <= max_error_limit).astype(float)
    valid_short_days = (daily_error_short <= max_error_limit).astype(float)

    # C. Calculate daily Dollar PnL
    pnl_long_spread = sum(shares_long[t].shift(1) * dollar_diffs[t] * np.sign(w) for t, w in weights.items())
    pnl_short_spread = sum(shares_short[t].shift(1) * dollar_diffs[t] * -np.sign(w) for t, w in weights.items())
    
    # D. Convert Dollar PnL to Percentage Returns
    ret_long_spread = pnl_long_spread / cap_long
    ret_short_spread = pnl_short_spread / cap_short
    
    # E. Map returns to signals AND apply the C++ Abort Mask
    active_long = (actual_pos == 1.0).astype(float) * valid_long_days
    active_short = (actual_pos == -1.0).astype(float) * valid_short_days
    
    raw_pnl = (active_long * ret_long_spread) + (active_short * ret_short_spread)
    
    # Subtract transaction costs
    transaction_costs = trade_actions * cost_per_trade
    net_pnl = raw_pnl.fillna(0.0) - transaction_costs

    # =====================================================================
    # 6. Calculate Cumulative Returns and Maximum Drawdown (MDD)
    # =====================================================================
    cum_pnl = net_pnl.cumsum()

    # cummax() tracks the "high water mark" (the peak)
    running_max = cum_pnl.cummax()

    # The drawdown is the current value minus the highest peak
    drawdowns = cum_pnl - running_max
    
    # Grab the absolute worst drop (convert to a positive number for easy reading)
    max_drawdown = abs(drawdowns.min())
    
    # 7. OOS Performance Metrics
    oos_sharpe = np.sqrt(252) * (net_pnl.mean() / net_pnl.std()) if net_pnl.std() > 0 else 0
    oos_p_val = adfuller(oos_spread)[1]
    
    # STRICT FILTER: 
    # 1. Must still be stationary Out-Of-Sample
    # 2. Must be profitable (Sharpe > 0.5)
    # 3. MDD must be less than 20% (0.20)
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
    folder = "robust_configs"
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
                "capital_reqs": {
                    "long_spread": {
                        "min_required": item['trade_caps'][1]['min_capital'],
                        "target_allocated": item['trade_caps'][1]['target_capital']
                    },
                    "short_spread": {
                        "min_required": item['trade_caps'][-1]['min_capital'],
                        "target_allocated": item['trade_caps'][-1]['target_capital']
                    }
                }
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
        # 2. Validate with Out-of-Sample data
        oos_metrics = validate_oos_advanced(df_test, train_res)
        
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
                "trade_caps": train_res['trade_caps']
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