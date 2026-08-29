#pragma once

// Copy to device_config.h, or use tools/configure.py. Never commit credentials.
#define PVC_WIFI_SSID ""
#define PVC_WIFI_PASSWORD ""
#define PVC_API_BASE_URL "https://api.example.com/v1"

// HTTPS requires the PEM root CA that signs PVC_API_BASE_URL.
// This is a public trust anchor, not a private key.
#define PVC_TLS_ROOT_CA_PEM ""

// Plain HTTP is rejected unless this is 1 AND the URL host is local/private.
#define PVC_ALLOW_HTTP_LOCAL_DEV 0
