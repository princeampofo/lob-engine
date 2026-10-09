# Data

The replay uses the free [LOBSTER](https://lobsterdata.com) sample files: one trading day
(2012-06-21) of Nasdaq order-level data for AAPL, AMZN, GOOG, INTC and MSFT.

The files are not committed (about 600 MB). To download all five stocks:

```
scripts/download_data.sh            # or e.g. scripts/download_data.sh AAPL MSFT
```

Or get them by hand:

1. Request the sample data on lobsterdata.com (free; you accept their terms of use),
   or use the copy on Hugging Face:
   [Ayrus-1007/lobster-data](https://huggingface.co/datasets/Ayrus-1007/lobster-data).
2. Put the **10-level** files for each stock in this folder.

Each stock comes as a pair of CSV files with the same number of rows:

```
AAPL_2012-06-21_34200000_57600000_message_10.csv    one event per row
AAPL_2012-06-21_34200000_57600000_orderbook_10.csv  top 10 levels after that event
```

Then replay one day and check every row:

```
./build/lob_replay data/AAPL_2012-06-21_34200000_57600000_message_10.csv \
                   data/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv
```

The test suite also replays every `*_message_10.csv` it finds here, and skips that test
when the folder is empty.
