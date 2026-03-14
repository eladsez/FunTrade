import requests
import io 

def refresh_sp500_json(output_file="s&p500.json"):
    print("[*] Updating S&P 500 universe...")
    url = 'https://en.wikipedia.org/wiki/List_of_S%26P_500_companies'
    headers = {'User-Agent': 'Mozilla/5.0'}

    try:
        response = requests.get(url, headers=headers)
        response.raise_for_status()

        # Wrap the text in io.StringIO to avoid pandas warnings
        tables = pd.read_html(io.StringIO(response.text))
        df = tables[0]
        
        # Yahoo Finance requires '-' instead of '.' for tickers like BRK.B
        df['Symbol'] = df['Symbol'].str.replace('.', '-', regex=False)
        
        # Create the sector dictionary
        sector_dict = df.groupby('GICS Sector')['Symbol'].apply(list).to_dict()
        
        # Also add the full list for a 'total' run
        sector_dict["s&p500"] = df['Symbol'].tolist()
        
        with open(output_file, 'w') as f:
            json.dump(sector_dict, f, indent=4)
        
        print(f"[!] {output_file} updated: {len(df)} tickers saved.")

    except Exception as e:
        print(f"[!] Error: {e}")

if __name__ == "__main__":
    refresh_sp500_json()