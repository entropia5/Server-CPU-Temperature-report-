# TemperatureBot

A small C++ temperature monitor for Raspberry Pi and Linux boards.
It reads the CPU temperature from `/sys/class/thermal/thermal_zone0/temp`
and sends Telegram notifications.

## Features

- Sends CPU temperature reports to Telegram
- Sends an alert when the CPU temperature is higher than `50.0 C`
- Marks the temperature as normal again at `50.0 C` or lower
- Sends regular status reports every 12 hours
- Checks the temperature every 30 seconds
- Uses a local `.env` file for Telegram credentials

## Requirements

- Linux with `/sys/class/thermal/thermal_zone0/temp`
- C++17 compiler
- libcurl development package
- Telegram bot token
- Telegram chat ID

On Raspberry Pi OS or Debian/Ubuntu:

```bash
sudo apt update
sudo apt install g++ libcurl4-openssl-dev
```

## Telegram Setup

1. Create a Telegram bot with `@BotFather`.
2. Copy the bot token.
3. Send any message to your bot.
4. Get your chat ID.

One common way to get the chat ID:

```bash
curl "https://api.telegram.org/botYOUR_BOT_TOKEN/getUpdates"
```

Look for the `chat.id` value in the response.

## Configuration

Create a `.env` file in the project directory:

```env
BOT_TOKEN=your_telegram_bot_token
CHAT_ID=your_chat_id
```

Do not commit `.env` to GitHub because it contains secrets.

## Build

```bash
g++ -std=c++17 temperature_bot.cpp -lcurl -pthread -o temperature_bot
```

## Run

```bash
./temperature_bot
```

The program prints basic status information to the console and sends
Telegram messages when needed.

## Current Thresholds

The current settings are defined in `temperature_bot.cpp`:

```cpp
const float TEMP_ALARM = 50.0f;
const float TEMP_NORMAL = 50.0f;
const auto REPORT_INTERVAL = std::chrono::hours(12);
```

Behavior:

- `50.0 C` and lower: normal
- Higher than `50.0 C`: alert
- Regular report: every 12 hours

## Run as a systemd Service

Create a service file:

```bash
sudo nano /etc/systemd/system/temperature-bot.service
```

Example service:

```ini
[Unit]
Description=TemperatureBot CPU temperature Telegram monitor
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/home/pi/TemperatureBot
ExecStart=/home/pi/TemperatureBot/temperature_bot
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

Update `WorkingDirectory` and `ExecStart` if your project is in a different
directory.

Enable and start the service:

```bash
sudo systemctl daemon-reload
sudo systemctl enable temperature-bot
sudo systemctl start temperature-bot
```

Check logs:

```bash
journalctl -u temperature-bot -f
```

## Files

- `temperature_bot.cpp` - main Telegram temperature monitor
- `example_temperature_bot_plus_fan.cpp` - example version with fan control logic

## Notes

- The first report is sent immediately after startup.
- Telegram messages use MarkdownV2 escaping.
- If the temperature file cannot be read, the program waits 5 seconds and tries again.
