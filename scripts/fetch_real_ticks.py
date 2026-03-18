import os
import sys
import json
import time
import gc
import pandas as pd
import numpy as np
import pytz
from datetime import datetime, timedelta
from tqdm import tqdm

from alpaca.data.historical import StockHistoricalDataClient
from alpaca.data.requests import StockTradesRequest, StockBarsRequest
from alpaca.data.timeframe import TimeFrame

# ==========================================
# CONFIGURATION LOADER
# ==========================================
def load_config(config_path):
    """Parses the strategy JSON config for backtesting parameters."""
    with open(config_path, 'r') as f:
        config = json.load(f)
    
    tickers = config.get("tickers", [])
    window_size = config.get("trading_params", {}).get("window_size", 20)
    
    # FIX: Look inside "backtest_periods" to find the "testing" block
    testing_cfg = config.get("backtest_periods", {}).get("testing", {})
    start_str = testing_cfg.get("start")
    end_str = testing_cfg.get("end")
    
    if not start_str or not end_str:
        raise ValueError("Config must contain 'backtest_periods.testing.start' and 'end' dates (YYYY-MM-DD).")
        
    start_date = datetime.strptime(start_str, "%Y-%m-%d").date()
    end_date = datetime.strptime(end_str, "%Y-%m-%d").date()
    
    return tickers, window_size, start_date, end_date

# ==========================================
# WARMUP BARS FETCHER
# ==========================================
def fetch_warmup_bars(client, tickers, backtest_start_date, window_size):
    """Downloads EOD bars for the warmup period and saves them to a separate CSV."""
    # We add 10 buffer days to ensure we have enough trading days despite weekends/holidays.
    calendar_days_back = (window_size * 2) + 10 
    warmup_start = backtest_start_date - timedelta(days=calendar_days_back)
    warmup_end = backtest_start_date - timedelta(days=1) 

    print(f"Fetching warmup daily bars from {warmup_start} to {warmup_end}...")

    request_params = StockBarsRequest(
        symbol_or_symbols=tickers,
        timeframe=TimeFrame.Day,
        start=warmup_start,
        end=warmup_end,
        feed="sip"
    )

    try:
        bars_response = client.get_stock_bars(request_params)
        all_bars = []

        for symbol, bars in bars_response.data.items():
            for bar in bars:
                all_bars.append({
                    "timestamp_ms": int(bar.timestamp.timestamp() * 1000),
                    "symbol": symbol,
                    "close": bar.close,
                    "volume": bar.volume
                })

        if not all_bars:
            print("Warning: No warmup bars found!")
            return

        df = pd.DataFrame(all_bars).sort_values("timestamp_ms")
        
        output_file = f"data/warmup_{'-'.join(tickers)}_{backtest_start_date.strftime('%Y.%m.%d')}.csv"
        os.makedirs("data", exist_ok=True)
        df.to_csv(output_file, index=False)
        print(f"Successfully saved {len(df)} warmup bars to {output_file}")
        
    except Exception as e:
        print(f"Error fetching warmup bars: {e}")

# ==========================================
# TICK DATA HELPERS
# ==========================================
def process_symbol(symbol, trades):
    """Vectorized processing for a single symbol."""
    if not trades:
        return pd.DataFrame()

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

# ==========================================
# TICK DATA FETCHER
# ==========================================
def fetch_real_tick_data(client, tickers, start_date, end_date):
    """Downloads tick data iteratively with chunking to prevent OOM errors."""
    total_days = (end_date - start_date).days + 1
    all_dfs = []

    os.makedirs("data", exist_ok=True)
    output_file = f"data/ticks_{'-'.join(tickers)}_{start_date.strftime('%Y.%m.%d')}_{end_date.strftime('%Y.%m.%d')}.csv"

    print(f"Fetching {total_days} days of tick data...")
    print(f"Data will be streamed to: {output_file}")
    
    ny_tz = pytz.timezone('America/New_York')
    current_date = start_date
    
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
                
                days_in_current_chunk += 1

                # CHUNKING & MEMORY CLEARING
                if days_in_current_chunk >= chunk_size_days and len(all_dfs) > 0:
                    chunk_df = pd.concat(all_dfs).sort_values("timestamp_ms")
                    chunk_df['trade_id'] = np.arange(global_trade_id, global_trade_id + len(chunk_df))
                    global_trade_id += len(chunk_df)
                    
                    mode = 'w' if is_first_chunk else 'a'
                    header = is_first_chunk
                    chunk_df[["timestamp_ms", "symbol", "side", "price", "volume", "trade_id"]].to_csv(output_file, mode=mode, header=header, index=False)
                    
                    days_in_current_chunk = 0
                    is_first_chunk = False

                    # Aggressively clear RAM
                    del chunk_df
                    del all_dfs
                    if 'trades_response' in locals():
                        del trades_response
                    gc.collect()
                    all_dfs = []
            
            current_date += timedelta(days=1)
            pbar.update(1)

    # FINAL DUMP
    if len(all_dfs) > 0:
        print("\nSaving final chunk...")
        chunk_df = pd.concat(all_dfs).sort_values("timestamp_ms")
        chunk_df['trade_id'] = np.arange(global_trade_id, global_trade_id + len(chunk_df))
        
        mode = 'w' if is_first_chunk else 'a'
        header = is_first_chunk
        chunk_df[["timestamp_ms", "symbol", "side", "price", "volume", "trade_id"]].to_csv(output_file, mode=mode, header=header, index=False)

    print(f"\nDone! Successfully downloaded all data to {output_file}.")

# ==========================================
# MAIN ENTRY POINT
# ==========================================
def main():
    if len(sys.argv) < 2:
        print("Usage: python fetch_real_ticks.py <path_to_config.json>")
        sys.exit(1)

    config_path = sys.argv[1]

    api_key = os.getenv("APCA_API_KEY_ID")
    secret_key = os.getenv("APCA_API_SECRET_KEY")
    
    if not api_key or not secret_key:
        print("Error: APCA_API_KEY_ID and APCA_API_SECRET_KEY environment variables must be set.")
        sys.exit(1)

    print(f"Loading backtest parameters from {config_path}...")
    tickers, window_size, start_date, end_date = load_config(config_path)
    
    print(f"Target Basket: {tickers}")
    print(f"Testing Window: {start_date} to {end_date}")
    
    client = StockHistoricalDataClient(api_key, secret_key)
    
    print("\n--- STEP 1: Fetching Warmup Data ---")
    fetch_warmup_bars(client, tickers, start_date, window_size)
    
    print("\n--- STEP 2: Fetching Real-Time Tick Data ---")
    fetch_real_tick_data(client, tickers, start_date, end_date)

if __name__ == "__main__":
    main()