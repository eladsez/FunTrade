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

warnings.filterwarnings("ignore")


def fetch_data(tickers, start_date, end_date):
    print(f"[*] Downloading {len(tickers)} tickers...")
    data = yf.download(tickers, start=start_date, end=end_date)['Close']
    return data.dropna()

def calculate_half_life(spread):
    spread_lag = spread.shift(1).dropna()
    spread_diff = spread.diff().dropna()
    X = sm.add_constant(spread_lag)
    model = sm.OLS(spread_diff, X).fit()
    lam = model.params.iloc[1]
    return -np.log(2) / lam if lam < 0 else np.inf

def find_best_params(spread, window_size):
    """ 
    Grid Search: Entry Z vs Exit Z using ROLLING statistics 
    to match the C++ engine logic.
    """
    entry_range = np.arange(1.5, 3.25, 0.25)
    exit_range = np.arange(-0.25, 0.75, 0.25)
    
    # Calculate rolling mean and std to avoid look-ahead bias
    # This mimics the C++ rolling window
    rolling_mu = spread.rolling(window=window_size).mean()
    rolling_std = spread.rolling(window=window_size).std()
    
    # Calculate rolling Z-Score
    z_spread = (spread - rolling_mu) / rolling_std
    
    # Drop NaNs created by the rolling window (the 'warm-up' period)
    z_spread = z_spread.dropna()
    valid_spread_diff = spread.diff().loc[z_spread.index]

    results = []

    for entry_z, exit_z in product(entry_range, exit_range):
        if exit_z >= entry_z: continue 

        # Vectorized simulation logic is faster, but a loop is clearer for state machines
        positions = []
        current_pos = 0
        
        for z in z_spread:
            if current_pos == 0:
                if z > entry_z: current_pos = -1
                elif z < -entry_z: current_pos = 1
            elif current_pos == -1: # Short position
                if z <= exit_z: current_pos = 0
            elif current_pos == 1: # Long position
                if z >= -exit_z: current_pos = 0
            positions.append(current_pos)

        pos_series = pd.Series(positions, index=z_spread.index)
        
        # Calculate PnL: Position of 'yesterday' * change in spread 'today'
        daily_pnl = pos_series.shift(1).fillna(0) * valid_spread_diff
        
        # Calculate Sharpe Ratio (Annualized)
        mean_pnl = daily_pnl.mean()
        std_pnl = daily_pnl.std()
        
        sharpe = np.sqrt(252) * (mean_pnl / std_pnl) if std_pnl > 0 else 0
        
        results.append({
            'entry': entry_z, 
            'exit': exit_z, 
            'sharpe': sharpe
        })

    if not results: return 2.0, 0.0, 0.0
    
    best = max(results, key=lambda x: x['sharpe'])
    return round(float(best['entry']), 2), round(float(best['exit']), 2), round(float(best['sharpe']), 3)


def test_basket(df, combo):
    subset_df = df[list(combo)]
    try:
        joh_res = coint_johansen(subset_df.values, 0, 1)
        raw_weights = joh_res.evec[:, 0]
        weights = {combo[i]: round(raw_weights[i]/raw_weights[0], 4) for i in range(len(combo))}
        
        spread = sum(subset_df[t] * weights[t] for t in combo)
        p_val = adfuller(spread)[1]
        hl = calculate_half_life(spread)

        # Gatekeeper
        if p_val > 0.05 or not (2 <= hl <= 60): return None

        # Define window size based on Half-Life (e.g., 2x HL)
        window_size = int(max(round(hl * 2), 20))

        # Pass the window_size to the grid search
        entry_z, exit_z, sharpe = find_best_params(spread, window_size)
        
        if sharpe <= 0.1: return None # Filter out low-performing noise

        return {
            "tickers": combo, "weights": weights, "p_value": p_val,
            "half_life": hl, "entry_z": entry_z, "exit_z": exit_z,
            "sharpe": sharpe, "rank_score": sharpe / (p_val + 0.001)
        }

    except: return None

def validate_oos_advanced(df_oos, train_res):
    """
    Returns OOS stats only if they are reasonably stable.
    """
    tickers = train_res['tickers']
    weights = train_res['weights']
    
    # Reconstruct spread on NEW data
    oos_spread = sum(df_oos[t] * weights[t] for t in tickers)
    
    # Calculate OOS Stats
    oos_p_val = adfuller(oos_spread)[1]
    oos_hl = calculate_half_life(oos_spread)
    
    # Stability Check: Half-life shouldn't explode (e.g., > 2x training HL)
    hl_ratio = oos_hl / train_res['half_life']
    is_stable_hl = 0.5 < hl_ratio < 2.5 
    
    is_robust = (oos_p_val < 0.05) and is_stable_hl

    return {
        "oos_p": oos_p_val,
        "oos_hl": oos_hl,
        "is_robust": is_robust,
        "hl_drift": hl_ratio
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
                "oos_hl": round(item['oos_hl'], 2),
                "sharpe": item['train_sharpe']
            },
            "trading_params": {
                "window_size": int(max(round(item['oos_hl'] * 2.0), 20)),
                "entry_z": item['entry_z'],
                "exit_z": item['exit_z']
            },
            "exported_at": datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        }
        
        file_path = os.path.join(folder, f"{basket_id}.json")
        with open(file_path, 'w') as f:
            json.dump(config, f, indent=4)

    print(f"\n[!] Successfully exported {len(robust_list)} configs to /{folder}")

def main():
    # Research Window: 5 Years
    end_date = datetime.now().strftime('%Y-%m-%d')
    start_date = (datetime.now() - timedelta(days=5*365)).strftime('%Y-%m-%d')
    
    # Top Tech/Semiconductors
    # universe = [
    #     'INTC', 'AMD', 'NVDA', 'TXN', 'QCOM', 
    #     'MU', 'AMAT', 'LRCX', 'ASML', 'AVGO',
    #     'TSM', 'NXPI', 'MRVL', 'ADI', 'KLAC',
    #     'WDC'
    # ]

    # Big Banks
    # universe = [
    #     'JPM', 'BAC', 'WFC', 'C', 'GS', 
    #     'MS', 'USB', 'PNC', 'TFC', 'BK'
    # ]

    # Energy - Big Oil
    # universe = [
    #     'XOM', 'CVX', 'COP', 'SLB', 'EOG', 
    #     'MPC', 'VLO', 'OXY', 'HAL', 'DVN', 'FANG'
    # ]

    # Consumer Staples
    # universe = [
    #     'PG', 'KO', 'PEP', 'COST', 'WMT', 
    #     'TGT', 'PM', 'MO', 'CL', 'KMB'
    # ]
     
    # Utilities
    universe = [
        'NEE', 'DUK', 'SO', 'AEP', 'D', 
        'EXC', 'PCG', 'XEL', 'ED', 'WEC'
    ]
    
    # 2. Fetch and Split
    df = fetch_data(universe, start_date, end_date)
    split_idx = int(len(df) * 0.8)
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
                    "train_p": train_res['p_value'],      # Renaming for export
                    "train_hl": train_res['half_life'],   # Renaming for export
                    "train_sharpe": train_res['sharpe'],
                    "oos_p": oos_metrics['oos_p'],
                    "oos_hl": oos_metrics['oos_hl'],
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

if __name__ == "__main__":
    main()