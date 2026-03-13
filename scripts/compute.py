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


def main():
    # Research Window: 5 Years
    end_date = datetime.now().strftime('%Y-%m-%d')
    start_date = (datetime.now() - timedelta(days=5*365)).strftime('%Y-%m-%d')
    
     # Define a focused universe (e.g., Top Tech/Semiconductors)
    # Trying to brute force all 500 will take too long, so we start with a sector.
    universe = [
        'INTC', 'AMD', 'NVDA', 'TXN', 'QCOM', 
        'MU', 'AMAT', 'LRCX', 'ASML', 'AVGO',
        'TSM', 'NXPI', 'MRVL', 'ADI', 'KLAC'
    ]
    df = fetch_data(universe, start_date, end_date)
    
    combos = list(combinations(df.columns, 3))
    print(f"[*] Brute-forcing {len(combos)} baskets")

    results_list = []
    for i, combo in enumerate(combos):
        if i % 10 == 0: print(f"    Searching... {i}/{len(combos)}")
        res = test_basket(df, combo)
        if res: results_list.append(res)

    if results_list:
        # Sort by the hybrid rank score
        best = max(results_list, key=lambda x: x['rank_score'])
        
        print("\n" + "🚀" * 20)
        print(f"OPTIMAL CONFIGURATION FOUND")
        print(f"BASKET: {best['tickers']}")
        print(f"ENTRY Z: {best['entry_z']} | EXIT Z: {best['exit_z']}")
        print(f"SHARPE: {best['sharpe']} | HALF-LIFE: {best['half_life']:.2f}")
        print(f"P: {best['p_value']:.6f}")
        print("🚀" * 20)

        config = {
            "basket_id": f"grid_opt_{datetime.now().strftime('%Y%m%d')}",
            "window_size": int(max(round(best['half_life'] * 2.0), 20)),
            "entry_z_score": best['entry_z'],
            "exit_z_score": best['exit_z'],
            "components": best['weights']
        }

        with open('basket_config.json', 'w') as f:
            json.dump(config, f, indent=4)
        print("\n[!] Config saved.")
    else:
        print("\n[!] No profitable stationary baskets found.")

if __name__ == "__main__":
    main()