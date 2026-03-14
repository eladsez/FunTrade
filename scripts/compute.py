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


def fetch_data(tickers, start_date, end_date):
    print(f"[*] Downloading {len(tickers)} tickers...")
    data = yf.download(tickers, start=start_date, end=end_date, threads=True)['Close']
    
    # Drop columns (tickers) that have more than 10% missing data
    threshold = len(data) * 0.9
    data = data.dropna(thresh=threshold, axis=1)
    
    # Now drop remaining rows with NaNs (holidays, etc.)
    clean_data = data.dropna()
    
    print(f"[*] Proceeding with {len(clean_data.columns)} valid tickers.")
    return clean_data

def calculate_half_life(spread):
    spread_lag = spread.shift(1).dropna()
    spread_diff = spread.diff().dropna()
    X = sm.add_constant(spread_lag)
    model = sm.OLS(spread_diff, X).fit()
    lam = model.params.iloc[1]
    return -np.log(2) / lam if lam < 0 else np.inf

def find_best_params(spread, window_size, cost_per_trade=0.001):
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


def test_basket(df, combo, cost_per_trade=0.001):
    subset_df = df[list(combo)]
    
    try:
        # Convert raw prices to log prices to ensure dollar-neutrality
        # and proportional capital allocation.
        log_df = np.log(subset_df)
        
        # Run Johansen on the log prices
        joh_res = coint_johansen(log_df.values, 0, 1)
        raw_weights = joh_res.evec[:, 0]
        weights = {combo[i]: round(raw_weights[i]/raw_weights[0], 4) for i in range(len(combo))}
        
        # Spread is now a log-spread
        spread = sum(log_df[t] * weights[t] for t in combo)
        p_val = adfuller(spread)[1]
        hl = calculate_half_life(spread)

        # Gatekeeper
        if p_val > 0.05 or not (2 <= hl <= 60): 
            return None

        window_size = int(max(round(hl * 2), 20))

        # Grid search will now operate on the log-spread
        entry_z, exit_z, sharpe = find_best_params(spread, window_size, cost_per_trade)
        
        # Filter out noise - require a solid baseline Sharpe
        if sharpe <= 0.1: 
            return None 

        return {
            "tickers": combo, 
            "weights": weights, 
            "p_value": p_val,
            "half_life": hl, 
            "entry_z": entry_z, 
            "exit_z": exit_z,
            "sharpe": sharpe
        }

    except Exception as e: 
        return None

def validate_oos_advanced(df_oos, train_res, cost_per_trade=0.001):
    """
    Simulates OOS performance using Log Prices and counts the number of trades.
    Disqualifies baskets that rely on a single lucky anomaly.
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

    # 3. Trading simulation
    pos = pd.Series(0, index=z_spread.index)
    current_pos = 0
    
    for i in range(len(z_spread)):
        z = z_spread.iloc[i]
        if current_pos == 0:
            if z > entry_z: current_pos = -1
            elif z < -entry_z: current_pos = 1
        elif current_pos == -1: 
            if z <= exit_z: current_pos = 0
        elif current_pos == 1:  
            if z >= -exit_z: current_pos = 0
            
        pos.iloc[i] = current_pos

    # 4. Count Trades to prevent the "1 lucky trade" illusion
    # A position change from 0 to 1, or 1 to 0, counts as 1 trade action.
    # Two actions = roughly one round-trip trade.
    trade_actions = pos.diff().abs().fillna(0)
    total_trade_actions = trade_actions[trade_actions > 0].count()
    round_trip_trades = int(total_trade_actions / 2)

    # STRICT FILTER: If the basket made fewer than 4 round-trips in a year, it's noise.
    if round_trip_trades < 4:
        return {"is_robust": False}

    # 5. Calculate returns and transaction costs
    # Since we are using log prices, spread_diff is approx the percentage return
    log_return = oos_spread.loc[z_spread.index].diff().shift(-1)
    raw_pnl = pos * log_return
    
    # Subtract flat percentage cost on every trade action
    transaction_costs = trade_actions * cost_per_trade
    net_pnl = raw_pnl - transaction_costs
    
    # 6. OOS Performance Metrics
    oos_sharpe = np.sqrt(252) * (net_pnl.mean() / net_pnl.std()) if net_pnl.std() > 0 else 0
    oos_p_val = adfuller(oos_spread)[1]
    
    # Robustness criteria: Stationary, profitable, and actively trading
    is_robust = (oos_p_val < 0.05) and (oos_sharpe > 0.5) 

    return {
        "oos_p": oos_p_val,
        "oos_sharpe": round(oos_sharpe, 3),
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
                "exit_z": item['exit_z']
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

def run_over_universe(universe):
    # Research Window: 5 Years
    end_date = datetime.now().strftime('%Y-%m-%d')
    start_date = (datetime.now() - timedelta(days=5*365)).strftime('%Y-%m-%d')

    # 2. Fetch and Split
    df = fetch_data(universe, start_date, end_date)

    # Check if we have enough columns (tickers) left after dropna()
    if df.empty or len(df.columns) < 2:
        print(f"[!] Critical Error: Not enough valid ticker data for this sector.")
        return

    split_idx = int(len(df) * 0.8)

    # Ensure the split actually leaves data in both sets
    if split_idx < 20 or (len(df) - split_idx) < 20:
        print(f"[!] Skipping sector: Insufficient data points for a 80/20 split.")
        return

    df_train = df.iloc[:split_idx]
    df_test = df.iloc[split_idx:]

    # Capture Date Ranges for the Config
    train_dates = (df_train.index[0].strftime('%Y-%m-%d'), df_train.index[-1].strftime('%Y-%m-%d'))
    test_dates = (df_test.index[0].strftime('%Y-%m-%d'), df_test.index[-1].strftime('%Y-%m-%d'))
    
    print(f"[*] Train Period: {train_dates[0]} to {train_dates[1]}")
    print(f"[*] Test Period:  {test_dates[0]} to {test_dates[1]}")
    
    combos = list(combinations(df.columns, 3))
    print(f"[*] Brute-forcing {len(combos)} baskets...")

    results_list = []
    for i, combo in enumerate(combos):
        if i % 50 == 0: print(f"    Searching... {i}/{len(combos)}")
        
        # 1. Train the basket
        train_res = test_basket(df_train, combo)
        
        if train_res:
            # 2. Validate with Out-of-Sample data
            oos_metrics = validate_oos_advanced(df_test, train_res)
            
            if oos_metrics['is_robust']:
                # 3. CRITICAL: Map the keys correctly for the export function
                export_data = {
                    "tickers": train_res['tickers'],
                    "weights": train_res['weights'],
                    "train_p": train_res['p_value'],      
                    "train_hl": train_res['half_life'], 
                    "train_sharpe": train_res['sharpe'],
                    "oos_p": oos_metrics['oos_p'],
                    "oos_sharpe": oos_metrics['oos_sharpe'],
                    "entry_z": train_res['entry_z'],
                    "exit_z": train_res['exit_z']
                }
                results_list.append(export_data)

    # 4. Final Export
    if results_list:
        export_robust_configs(results_list, train_dates, test_dates)
        # Optional: Print the top survivor
        best_oos = min(results_list, key=lambda x: x['oos_p'])
        print(f"\n🚀 BEST ROBUST BASKET: {best_oos['tickers']} (OOS P: {best_oos['oos_p']:.6f})")
    else:
        print("\n[!] No baskets survived the stability tests.")


def main():
    universes = load_sector_universe("./s&p500.json")

    for sector, symbols in universes.items():
        print(f"====== Running on {sector} sector ====== ")
        run_over_universe(symbols)

if __name__ == "__main__":
    main()