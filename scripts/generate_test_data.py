import yfinance as yf
import pandas as pd
import random
import os

def generate_csv():
    # Configuration
    tickers = ["AMZN", "CMG", "EXPE"]
    start_date = "2025-03-14"
    end_date = "2026-03-14" # Exclusive in yfinance, so this includes the 13th
    output_file = "data/testing_ticks.csv"

    print(f"Fetching historical data for {tickers} from {start_date} to {end_date}...")
    
    # Download data
    data = yf.download(tickers, start=start_date, end=end_date, group_by='ticker')
    
    ticks = []
    trade_id_counter = 10000

    # Process each ticker
    for symbol in tickers:
        df = data[symbol].dropna()
        
        for date, row in df.iterrows():
            # Get the midnight timestamp of the day in milliseconds
            base_ts_ms = int(date.timestamp() * 1000)
            
            # Simulate 4 intraday ticks to give the strategy realistic movement
            # 1. Market Open (9:30 AM EST -> approx +9.5 hours)
            # 2. Daily High/Low (11:00 AM EST)
            # 3. Daily Low/High (1:00 PM EST)
            # 4. Market Close (4:00 PM EST -> approx +16 hours)
            
            # Randomize whether the high or low came first that day
            high_first = random.choice([True, False])
            
            pseudo_ticks = [
                {"offset": 9.5 * 3600000, "price": row['Open'], "side": "buy"},
                {"offset": 11.0 * 3600000, "price": row['High'] if high_first else row['Low'], "side": "buy" if high_first else "sell"},
                {"offset": 13.0 * 3600000, "price": row['Low'] if high_first else row['High'], "side": "sell" if high_first else "buy"},
                {"offset": 16.0 * 3600000, "price": row['Close'], "side": "sell"},
                {"offset": 16.0 * 3600000 + 1000, "price": row['Close'], "side": "bar"}
            ]
            
            avg_vol = int(row['Volume'] / 4) if row['Volume'] > 0 else 100
            
            for pt in pseudo_ticks:
                ticks.append({
                    "timestamp_ms": base_ts_ms + int(pt["offset"]),
                    "symbol": symbol,
                    "side": pt["side"],
                    "price": round(pt["price"], 2),
                    "volume": avg_vol,
                    "trade_id": trade_id_counter
                })
                trade_id_counter += 1

    # Sort all ticks chronologically so the DataFeed plays them back in perfect order
    print("Sorting ticks chronologically...")
    ticks.sort(key=lambda x: x["timestamp_ms"])

    # Ensure the data directory exists
    os.makedirs("data", exist_ok=True)

    # Write to CSV
    print(f"Writing {len(ticks)} ticks to {output_file}...")
    with open(output_file, 'w') as f:
        f.write("timestamp_ms,symbol,side,price,volume,trade_id\n")
        for t in ticks:
            f.write(f"{t['timestamp_ms']},{t['symbol']},{t['side']},{t['price']},{t['volume']},{t['trade_id']}\n")
            
    print("Done! Your backtest data is ready.")

if __name__ == "__main__":
    generate_csv()