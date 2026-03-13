import numpy as np
import pandas as pd
import yfinance as yf
import statsmodels.api as sm
from statsmodels.tsa.stattools import adfuller
from statsmodels.tsa.vector_ar.vecm import coint_johansen
from itertools import combinations
from datetime import datetime, timedelta
import json
import warnings

# Suppress warnings for a clean CLI output
warnings.filterwarnings("ignore")

# --- CORE MATH FUNCTIONS ---

def fetch_data(tickers, start_date, end_date):
    print(f"[*] Fetching data for {len(tickers)} tickers from {start_date} to {end_date}...")
    data = yf.download(tickers, start=start_date, end=end_date)['Close']
    return data.dropna()

def calculate_half_life(spread):
    spread_lag = spread.shift(1).dropna()
    spread_diff = spread.diff().dropna()
    X = sm.add_constant(spread_lag)
    model = sm.OLS(spread_diff, X).fit()
    lam = model.params.iloc[1]
    return -np.log(2) / lam if lam < 0 else np.inf

def find_robust_z_score(spread):
    """ Script 2's Simulation: Finds a stable 'plateau' of profitability """
    z_range = np.arange(1.5, 3.25, 0.25)
    sharpe_results = {}
    mu, std = spread.mean(), spread.std()
    z_spread = (spread - mu) / std

    for z in z_range:
        pos = pd.Series(0, index=spread.index)
        current_pos = 0
        for i in range(len(z_spread)):
            if current_pos == 0:
                if z_spread.iloc[i] > z: current_pos = -1
                elif z_spread.iloc[i] < -z: current_pos = 1
            else: # Exit at mean reversion (0.0)
                if (current_pos == -1 and z_spread.iloc[i] <= 0) or \
                   (current_pos == 1 and z_spread.iloc[i] >= 0):
                    current_pos = 0
            pos.iloc[i] = current_pos

        daily_pnl = pos.shift(1) * spread.diff()
        sharpe = np.sqrt(252) * (daily_pnl.mean() / daily_pnl.std()) if daily_pnl.std() != 0 else 0
        sharpe_results[z] = sharpe

    # Find the best Z where neighbors are also profitable (Robustness Check)
    best_z, max_smoothed_sharpe = 2.0, -np.inf
    keys = sorted(sharpe_results.keys())
    for i in range(1, len(keys) - 1):
        smoothed = (sharpe_results[keys[i-1]] + sharpe_results[keys[i]] + sharpe_results[keys[i+1]]) / 3
        if smoothed > max_smoothed_sharpe:
            max_smoothed_sharpe, best_z = smoothed, keys[i]
            
    return round(float(best_z), 2), round(float(sharpe_results.get(best_z, 0)), 3)

def test_basket(df, subset_tickers):
    """ Evaluates a combination using a hybrid of Stats + Backtest """
    subset_df = df[list(subset_tickers)]
    try:
        # Johansen Cointegration
        joh_res = coint_johansen(subset_df.values, 0, 1)
        raw_weights = joh_res.evec[:, 0]
        if raw_weights[0] == 0: return None
        
        # Normalize weights relative to the first asset
        norm_weights = {subset_tickers[i]: round(raw_weights[i]/raw_weights[0], 4) for i in range(len(subset_tickers))}
        
        # Build Spread
        spread = pd.Series(0.0, index=subset_df.index)
        for t in subset_tickers:
            spread += subset_df[t] * norm_weights[t]
        
        # ADF Test (Stationarity)
        p_val = adfuller(spread)[1]
        hl = calculate_half_life(spread)

        # --- FILTERS ---
        # 1. Must be stationary
        # 2. Half-life must be tradable (between 2 days and 2 months)
        if p_val > 0.05 or not (2 <= hl <= 50):
            return None

        # --- OPTIMIZATION ---
        opt_z, sharpe = find_robust_z_score(spread)
        
        # We only want profitable baskets
        if sharpe <= 0: return None

        return {
            "tickers": subset_tickers,
            "weights": norm_weights,
            "p_value": p_val,
            "half_life": hl,
            "opt_z": opt_z,
            "sharpe": sharpe,
            "rank_score": sharpe / (p_val + 0.0001) # Hybrid Ranking
        }
    except:
        return None

# --- MAIN EXECUTION LOOP ---

def main():
    # Setup
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
    
    all_combos = list(combinations(df.columns, 3))
    print(f"[*] Starting search through {len(all_combos)} combinations...")

    best_basket = None
    max_score = -np.inf

    for i, combo in enumerate(all_combos):
        if i % 20 == 0:
            print(f"    Progress: {i}/{len(all_combos)}...")

        result = test_basket(df, combo)
        
        if result and result['rank_score'] > max_score:
            max_score = result['rank_score']
            best_basket = result

    if best_basket:
        print("\n" + "="*40)
        print("🥇 SUPER-BASKET FOUND")
        print(f"Tickers: {best_basket['tickers']}")
        print(f"Sharpe Ratio: {best_basket['sharpe']}")
        print(f"P-Value: {best_basket['p_value']:.5f}")
        print(f"Half-Life: {best_basket['half_life']:.2f} days")
        print(f"Optimal Entry Z: {best_basket['opt_z']}")
        print("="*40)

        # Export for FunTrade Bot
        # Window size is usually 2x half-life to capture the full cycle
        window = int(max(round(best_basket['half_life'] * 2.0), 20))
        
        config = {
            "basket_id": f"hybrid_opt_{datetime.now().strftime('%Y%m%d')}",
            "window_size": window,
            "entry_z_score": best_basket['opt_z'],
            "exit_z_score": 0.5,
            "components": best_basket['weights']
        }

        with open('basket_config.json', 'w') as f:
            json.dump(config, f, indent=4)
        print("\n[!] Configuration exported to basket_config.json")
    else:
        print("\n[!] No basket met the statistical and profitability requirements.")

if __name__ == "__main__":
    main()