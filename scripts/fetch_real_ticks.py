import os
import pandas as pd
import numpy as np
from alpaca.data.historical import StockHistoricalDataClient
from alpaca.data.requests import StockTradesRequest
from datetime import datetime
from tqdm import tqdm
from datetime import timedelta
import pytz
import time

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
    
    tickers = ["AON", "MA", "WFC"]
    start_date = datetime(2025, 3, 1).date()
    end_date = datetime(2026, 3, 2).date()
    
    total_days = (end_date - start_date).days + 1
    all_dfs = []

    os.makedirs("data", exist_ok=True)
    output_file = f"data/ticks_{'-'.join(tickers)}_{start_date.strftime('%Y.%m.%d')}_{end_date.strftime('%Y.%m.%d')}.csv"

    print(f"Fetching {total_days} days of tick data...")
    print(f"Data will be streamed to: {output_file}")
    
    ny_tz = pytz.timezone('America/New_York')
    current_date = start_date
    
    # --- CHUNKING VARIABLES ---
    chunk_size_days = 10
    days_in_current_chunk = 0
    global_trade_id = 1
    is_first_chunk = True
    
    with tqdm(total=total_days, desc="Downloading Days") as pbar:
        while current_date <= end_date:
            if current_date.weekday() < 5:
                
                day_start = ny_tz.localize(datetime.combine(current_date, datetime.min.time()).replace(hour=9, minute=30))
                day_end = ny_tz.localize(datetime.combine(current_date, datetime.min.time()).replace(hour=16, minute=0))
                
                request_params = StockTradesRequest(
                    symbol_or_symbols=tickers,
                    start=day_start,
                    end=day_end,
                    feed="sip" 
                )
                
                success = False
                while not success:
                    try:
                        trades_response = client.get_stock_trades(request_params)
                        
                        for symbol, trades in trades_response.data.items():
                            if trades:
                                all_dfs.append(process_symbol(symbol, trades))
                                
                        success = True 
                        
                    except Exception as e:
                        error_msg = str(e).lower()
                        if "429" in error_msg or "rate limit" in error_msg or "connection" in error_msg:
                            print(f"\n[Warning] Rate limit or network drop on {current_date}. Sleeping for 60s...")
                            time.sleep(60)
                        else:
                            print(f"\n[Fatal Error] on {current_date}: {e}")
                            success = True
                
                # Increment our chunk counter (only count valid market days)
                days_in_current_chunk += 1

                # --- CHUNKING LOGIC: DUMP TO DISK EVERY 10 DAYS ---
                if days_in_current_chunk >= chunk_size_days and len(all_dfs) > 0:
                    chunk_df = pd.concat(all_dfs).sort_values("timestamp_ms")
                    
                    # Assign continuous trade IDs
                    chunk_df['trade_id'] = np.arange(global_trade_id, global_trade_id + len(chunk_df))
                    global_trade_id += len(chunk_df)
                    
                    # Write to CSV (Write mode 'w' for the first chunk to create headers, append 'a' for the rest)
                    mode = 'w' if is_first_chunk else 'a'
                    header = is_first_chunk
                    
                    chunk_df[["timestamp_ms", "symbol", "side", "price", "volume", "trade_id"]].to_csv(output_file, mode=mode, header=header, index=False)
                    
                    # Reset memory and state for the next chunk
                    all_dfs = []
                    days_in_current_chunk = 0
                    is_first_chunk = False
            
            current_date += timedelta(days=1)
            pbar.update(1)

    # --- FINAL DUMP: Save any remaining days that didn't reach the 10-day limit ---
    if len(all_dfs) > 0:
        print("\nSaving final chunk...")
        chunk_df = pd.concat(all_dfs).sort_values("timestamp_ms")
        chunk_df['trade_id'] = np.arange(global_trade_id, global_trade_id + len(chunk_df))
        
        mode = 'w' if is_first_chunk else 'a'
        header = is_first_chunk
        chunk_df[["timestamp_ms", "symbol", "side", "price", "volume", "trade_id"]].to_csv(output_file, mode=mode, header=header, index=False)

    print(f"\nDone! Successfully downloaded all data to {output_file}.")

if __name__ == "__main__":
    fetch_real_tick_data()