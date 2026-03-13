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

# Suppress warnings from statsmodels for clean output
warnings.filterwarnings("ignore")

def fetch_data(tickers, start_date, end_date):
    print(f"Fetching 5-year data for {len(tickers)} tickers...")
    data = yf.download(tickers, start=start_date, end=end_date)['Close']
    return data.dropna()

def calculate_half_life(spread):
    spread_lag = spread.shift(1).dropna()
    spread_diff = spread.diff().dropna()
    
    X = sm.add_constant(spread_lag)
    model = sm.OLS(spread_diff, X).fit()
    lam = model.params.iloc[1]
    
    if lam >= 0:
        return np.inf
        
    return -np.log(2) / lam

def test_basket(df, subset_tickers):
    """
    Runs Johansen and ADF tests on a specific subset of tickers.
    Returns a dictionary with results if successful, otherwise None.
    """
    subset_df = df[list(subset_tickers)]
    
    # Run Johansen test
    try:
        johansen_res = coint_johansen(subset_df.values, det_order=0, k_ar_diff=1)
    except Exception:
        return None

    # Get the best eigenvector (weights) and normalize
    raw_weights = johansen_res.evec[:, 0]
    if raw_weights[0] == 0:
        return None
        
    normalized_weights = raw_weights / raw_weights[0]
    weights_dict = {subset_tickers[i]: round(normalized_weights[i], 4) for i in range(len(subset_tickers))}
    
    # Calculate historical spread
    spread = pd.Series(0.0, index=subset_df.index)
    for ticker in subset_tickers:
        spread += subset_df[ticker] * weights_dict[ticker]
        
    # Run ADF test to confirm stationarity
    try:
        adf_res = adfuller(spread)
        p_value = adf_res[1]
    except Exception:
        return None
        
    # Calculate half-life
    half_life = calculate_half_life(spread)
    
    return {
        "tickers": subset_tickers,
        "weights": weights_dict,
        "p_value": p_value,
        "half_life": half_life
    }

def main():
    # Set dates dynamically: Today and 5 years ago
    end_date = datetime.now().strftime('%Y-%m-%d') # E.g., 2026-03-12
    start_date = (datetime.now() - timedelta(days=5*365)).strftime('%Y-%m-%d')
    
    print(f"Research period: {start_date} to {end_date}")
    
    # Define a focused universe (e.g., Top Tech/Semiconductors)
    # Trying to brute force all 500 will take too long, so we start with a sector.
    universe = [
        'INTC', 'AMD', 'NVDA', 'TXN', 'QCOM', 
        'MU', 'AMAT', 'LRCX', 'ASML', 'AVGO',
        'TSM', 'NXPI', 'MRVL', 'ADI', 'KLAC'
    ]
    
    # Fetch all data once
    df = fetch_data(universe, start_date, end_date)
    
    basket_size = 3
    all_combinations = list(combinations(df.columns, basket_size))
    print(f"Total combinations to test: {len(all_combinations)}")
    
    best_basket = None
    best_p_value = 1.0
    
    # Brute-force search through combinations
    for i, combo in enumerate(all_combinations):
        if i % 100 == 0 and i > 0:
            print(f"Tested {i}/{len(all_combinations)} combinations...")
            
        result = test_basket(df, combo)
        if not result:
            continue
            
        # We want stationary baskets (p < 0.05) with a tradable half-life (e.g., between 1 and 60 days)
        if result['p_value'] < best_p_value and 1 <= result['half_life'] <= 60:
            best_p_value = result['p_value']
            best_basket = result

    if best_basket and best_p_value < 0.05:
        print("\n=== OPTIMAL BASKET FOUND ===")
        print(f"Tickers: {best_basket['tickers']}")
        print(f"P-Value: {best_basket['p_value']:.6f}")
        print(f"Half-Life: {best_basket['half_life']:.2f} days")
        
        # Calculate window size based on half-life
        window_size = int(max(round(best_basket['half_life'] * 1.5), 5))
        
        # Export to JSON
        config = {
            "basket_id": "auto_generated_best_basket",
            "window_size": window_size,
            "entry_z_score": 2.0,
            "exit_z_score": 0.5,
            "components": best_basket['weights']
        }
        
        with open('basket_config.json', 'w') as f:
            json.dump(config, f, indent=4)
        print("Saved to basket_config.json")
    else:
        print("\nNo stationary basket found meeting the criteria. Try a different universe.")

if __name__ == "__main__":
    main()