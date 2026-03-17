import os
import pandas as pd
import numpy as np
from alpaca.data.historical import StockHistoricalDataClient
from alpaca.data.requests import StockTradesRequest
from datetime import datetime
from tqdm import tqdm
from datetime import timedelta

def process_symbol(symbol, trades):
    """Vectorized processing for a single symbol."""
    if not trades:
        return pd.DataFrame()

    # Wrap the list comprehension in tqdm to see per-symbol processing
    # (Optional: only useful if symbols have millions of trades)
    df = pd.DataFrame([
        {
            "timestamp_ms": int(t.timestamp.timestamp() * 1000),
            "symbol": symbol,
            "price": t.price,
            "volume": t.size,
        } for t in trades
    ])

    price_diff = df['price'].diff().fillna(0)
    df['side'] = np.where(price_diff >= 0, 'buy', 'sell')

    df['date'] = pd.to_datetime(df['timestamp_ms'], unit='ms').dt.date
    eod_bars = df.groupby('date').tail(1).copy()
    eod_bars['timestamp_ms'] += 1000
    eod_bars['side'] = 'bar'
    eod_bars['volume'] = 0

    return pd.concat([df, eod_bars], ignore_index=True)

def fetch_real_tick_data():
    api_key = os.getenv("APCA_API_KEY_ID")
    secret_key = os.getenv("APCA_API_SECRET_KEY")
    client = StockHistoricalDataClient(api_key, secret_key)
    
    tickers = ["AMZN", "CMG", "EXPE"]
    start_date = datetime(2025, 3, 14).date()
    end_date = datetime(2026, 3, 2).date()
    
    # Calculate total days for the progress bar
    total_days = (end_date - start_date).days + 1
    all_dfs = []

    print(f"Fetching {total_days} days of tick data...")
    
    # Use tqdm on the date range so you see progress every day
    current_date = start_date
    with tqdm(total=total_days, desc="Downloading Days") as pbar:
        while current_date <= end_date:
            # Define the specific window for this day
            day_start = datetime.combine(current_date, datetime.min.time()).replace(hour=9, minute=30)
            day_end = datetime.combine(current_date, datetime.min.time()).replace(hour=16, minute=0)
            
            # Skip weekends
            if current_date.weekday() < 5:
                request_params = StockTradesRequest(
                    symbol_or_symbols=tickers,
                    start=day_start,
                    end=day_end
                )
                
                try:
                    trades_response = client.get_stock_trades(request_params)
                    
                    # Process this day's data immediately
                    for symbol, trades in trades_response.data.items():
                        if trades:
                            all_dfs.append(process_symbol(symbol, trades))
                except Exception as e:
                    print(f"Error on {current_date}: {e}")
            
            current_date += timedelta(days=1)
            pbar.update(1)

    if not all_dfs:
        print("No data found!")
        return

    print("\nMerging and sorting master timeline...")
    final_df = pd.concat(all_dfs).sort_values("timestamp_ms")
    final_df['trade_id'] = np.arange(1, len(final_df) + 1)

    output_file = "data/real_ticks_fast.csv"
    os.makedirs("data", exist_ok=True)
    final_df[["timestamp_ms", "symbol", "side", "price", "volume", "trade_id"]].to_csv(output_file, index=False)
    
    print(f"Done! Created {output_file} with {len(final_df)} rows.")

if __name__ == "__main__":
    fetch_real_tick_data()