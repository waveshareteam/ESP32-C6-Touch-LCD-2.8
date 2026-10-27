# AIChats Voice Conversation

[English](README.md) | [简体中文](README_ZH.md)

## What It Does

Talk to Xiaozhi from the factory desktop, with a simple smile and connection status on screen.

## Hardware Required

The board, a connected speaker and Wi-Fi with internet access; use the onboard microphone.

## How to Run

Start the [factory desktop](../../README.md), connect Wi-Fi in Settings and open **AIChats**. Follow the on-screen activation instructions on first use, then say “你好小智” to start. You can also tap the smile to start or stop a conversation.

## Expected Result

The speaker plays Xiaozhi's reply while the screen shows a static smile and the current status. Try saying “把音量调到80” or “把亮度调到50” to adjust volume or brightness; swipe up from the bottom center to return to the desktop.

## Troubleshooting

- **Cannot connect or activate:** confirm internet access and follow the on-screen prompt and [Wi-Fi guide](../../USER_GUIDE.md#connecting-wi-fi).
- **No reply audio:** check the speaker and Settings volume. When uploading from source, use `idf.py flash` to include the required speech model; uploading only the application BIN is insufficient.
