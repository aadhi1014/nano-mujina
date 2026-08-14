# Mujina Fleet Monitor Dashboard

This is a standalone, lightweight, zero-dependency cyberpunk-themed dashboard for monitoring and managing `mujina-minerd` instances (or any mining hardware/daemons exposing a matching REST API).

## What's Included
* `dashboard.py`: A lightweight Python backend server (acting as an API proxy and settings manager).
* `dashboard.html`: The client-side single-page app containing all the styling, layout, charts, and interaction logic.

## Prerequisites
* Python 3.x (no external packages or pip modules needed).

## Configuration
Before running, open `dashboard.py` and update the `API_ROOT` variable at the top of the file to point to your miner daemon's REST API endpoint (default is `http://127.0.0.1:7785/api/v0`):

```python
API_ROOT = "http://your-miner-ip:7785/api/v0"
```

## Running the Dashboard
1. Open a terminal/command prompt in this folder.
2. Run the dashboard server:
   ```bash
   python dashboard.py
   ```
3. Open your browser and navigate to:
   ```
   http://127.0.0.1:8088
   ```

## Features
* **Zero Dependency**: Runs on plain Python standard libraries.
* **Aggregated Fleet Overview**: Live charts and summaries of aggregate hashrates and share submissions.
* **Per-board Tabs**: Detailed breakdown of chip hashrates, temperatures (ASIC and VR), voltage, current, and fan speeds.
* **Board Control**: Set auto/manual fan curves and adjust autotuning profiles directly from the client.
