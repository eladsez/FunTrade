#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

using json = nlohmann::json;

// Fast conversion from Alpaca's RFC3339 format to Epoch Milliseconds
uint64_t parseAlpacaTimestamp(const std::string& timestamp_str) {
    std::tm tm = {};
    int ms = 0;
    
    // Parse format: "2024-03-14T13:30:05.123456Z"
    // sscanf is significantly faster than std::get_time for millions of rows
    int year, month, day, hour, min, sec, micro;
    if (sscanf(timestamp_str.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", 
               &year, &month, &day, &hour, &min, &sec, &micro) == 7) {
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        ms = micro / 1000;
    }

    // Convert to time_t (UTC)
#if defined(_WIN32)
    time_t time = _mkgmtime(&tm);
#else
    time_t time = timegm(&tm);
#endif

    return (static_cast<uint64_t>(time) * 1000) + ms;
}

void fetchDay(const std::string& date_str, const std::string& symbols, 
              const std::string& api_key, const std::string& secret_key, 
              std::ofstream& csv_file, uint64_t& trade_id_counter) {
    
    std::string base_url = "https://data.alpaca.markets/v2/stocks/trades";
    std::string page_token = "";
    
    // Set explicit market hours for New York (Assuming EDT -04:00 for this example)
    // Note: You must adjust the offset to -05:00 if querying winter months (EST)
    std::string start_time = date_str + "T09:30:00-04:00";
    std::string end_time = date_str + "T16:00:00-04:00";

    std::cout << "[Fetcher] Processing Market Day: " << date_str << std::endl;

    // Track the last seen price to generate the EOD bar
    std::map<std::string, double> last_prices;
    std::map<std::string, double> previous_tick_prices;
    uint64_t day_end_timestamp_ms = 0;

    while (true) {
        cpr::Parameters params = {
            {"symbols", symbols},
            {"start", start_time},
            {"end", end_time},
            {"feed", "sip"},
            {"limit", "10000"} // Max allowed per request to minimize HTTP overhead
        };

        if (!page_token.empty()) {
            params.Add({"page_token", page_token});
        }

        cpr::Response r = cpr::Get(cpr::Url{base_url},
                                   cpr::Header{{"APCA-API-KEY-ID", api_key},
                                               {"APCA-API-SECRET-KEY", secret_key}},
                                   params);

        if (r.status_code != 200) {
            std::cerr << "[Error] HTTP " << r.status_code << " : " << r.text << std::endl;
            break;
        }

        auto response_json = json::parse(r.text);
        
        // Ensure there is data for this chunk
        if (!response_json.contains("trades") || response_json["trades"].is_null()) {
            break;
        }

        for (auto& [symbol, trades] : response_json["trades"].items()) {
            
            // Initialize previous price tracker if new symbol
            if (previous_tick_prices.find(symbol) == previous_tick_prices.end()) {
                previous_tick_prices[symbol] = 0.0;
            }

            for (const auto& trade : trades) {
                std::string ts_str = trade["t"];
                uint64_t ts_ms = parseAlpacaTimestamp(ts_str);
                double price = trade["p"];
                double volume = trade["s"];
                
                // Infer taker side based on tick direction (uptick = buy, downtick = sell)
                std::string side = (price >= previous_tick_prices[symbol]) ? "buy" : "sell";
                
                previous_tick_prices[symbol] = price;
                last_prices[symbol] = price; // Update the EOD price tracker
                
                // Keep track of the highest timestamp to append the bar slightly after the last trade
                if (ts_ms > day_end_timestamp_ms) {
                    day_end_timestamp_ms = ts_ms;
                }

                // Dump directly to disk instantly
                csv_file << ts_ms << "," 
                         << symbol << "," 
                         << side << "," 
                         << price << "," 
                         << volume << ",trd_" 
                         << trade_id_counter++ << "\n";
            }
        }

        if (response_json.contains("next_page_token") && !response_json["next_page_token"].is_null()) {
            page_token = response_json["next_page_token"].get<std::string>();
        } else {
            break; // No more pages for this day
        }
    }

    // Inject the EOD Bar for the strategy state machine
    if (!last_prices.empty()) {
        uint64_t bar_timestamp = day_end_timestamp_ms + 1000; // 1 second after last trade
        for (const auto& [symbol, close_price] : last_prices) {
            csv_file << bar_timestamp << "," 
                     << symbol << "," 
                     << "bar" << "," 
                     << close_price << "," 
                     << "0,eod_bar_" << date_str << "\n";
        }
        std::cout << "[Fetcher] EOD Bars injected successfully for " << date_str << std::endl;
    }
}

int main() {
    // You should preferably load these from environment variables
    std::string api_key = std::getenv("APCA_API_KEY_ID") ? std::getenv("APCA_API_KEY_ID") : "YOUR_API_KEY";
    std::string secret_key = std::getenv("APCA_API_SECRET_KEY") ? std::getenv("APCA_API_SECRET_KEY") : "YOUR_SECRET_KEY";

    std::string symbols = "AMZN,CMG,EXPE";
    
    // Simplified list of market days to fetch
    // In a production tool, you would generate this dynamically while skipping weekends
    std::vector<std::string> trading_days = {
        "2026-02-03",
        "2026-02-04",
        "2026-02-05"
    };

    std::string output_file = "data/cpp_testing_ticks.csv";
    std::ofstream csv_file(output_file, std::ios::out);
    
    if (!csv_file.is_open()) {
        std::cerr << "[Error] Failed to open output file: " << output_file << std::endl;
        return 1;
    }

    // Write the exact header required by CsvDataFeed.hpp
    csv_file << "timestamp_ms,symbol,side,price,volume,trade_id\n";

    uint64_t trade_id_counter = 1;

    for (const std::string& day : trading_days) {
        fetchDay(day, symbols, api_key, secret_key, csv_file, trade_id_counter);
    }

    csv_file.close();
    std::cout << "[Success] Full tick timeline exported to " << output_file << std::endl;

    return 0;
}