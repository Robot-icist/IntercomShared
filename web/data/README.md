# Intercom Web Client Files

These files are the web interface for the browser-based intercom client.
They can be uploaded to the ESP32's LittleFS filesystem using:

```bash
pio run --target uploadfs
```

Or they can be served from the embedded fallback (compiled into firmware).

## Files
- `index.html` - Main web page
- `style.css` - Styles
- `app.js` - Client-side JavaScript (audio capture, WebSocket, ADPCM codec)
- `manifest.json` - PWA manifest

## Customization
Edit these files to customize the web interface. Changes take effect after
uploading the filesystem image (no firmware recompile needed).

## Access
After flashing, access the web interface at:
- http://<device-ip>/
- http://<device-name>-web.local/ (mDNS)
