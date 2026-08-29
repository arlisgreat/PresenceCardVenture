# Web user session boundary

## Problem

The web client currently repeats the literal `demo-token` in JSON requests, photo uploads, and protected image downloads. That is sufficient for a single seeded demo account, but it couples every feature to the demo authentication mechanism and makes a later real login integration a cross-application rewrite.

## Decision

Introduce one browser session module and route every user-authenticated fetch through it.

- The session module owns the current user bearer token.
- It restores an explicitly saved token from browser storage.
- It otherwise uses `VITE_PRESENCE_USER_TOKEN` when configured.
- It falls back to `demo-token` only to keep the local seeded demo immediately runnable.
- Device credentials remain explicit function arguments because they represent an ESP32/device identity, not the signed-in web user.
- The fetch adapter adds the user authorization header without overwriting caller headers and supports binary and JSON requests.

This is an integration boundary, not a claim that production authentication exists. A production deployment must replace the demo token source with a real login/session issuer and should prefer a secure HttpOnly cookie where the deployment topology allows it.

## Alternatives considered

1. Pass the user token into every API helper. This is explicit but spreads session plumbing through every view and keeps protected images as a special case.
2. Implement cookie login immediately. This is the desired production destination, but it requires account storage, credential flows, CSRF policy, expiry, and recovery. It is too broad for this increment.
3. Centralize authenticated fetch now. This removes the current coupling while preserving the working demo and gives the later auth provider one replacement point. This is the selected approach.

## Data flow

1. A web feature calls an API helper or requests a protected image.
2. The authenticated fetch adapter resolves the current browser session.
3. The adapter adds `Authorization: Bearer <token>` and sends the request.
4. Existing API authorization continues to validate the token.
5. Device API helpers bypass the user session adapter and continue to use their device token.

## Failure behavior

- Missing or invalid user credentials remain visible as API `401` errors; helpers must not fabricate a successful response.
- Network-only demo fallbacks retain their current behavior where already documented.
- Persisted blank or malformed session values are ignored and the seeded demo fallback is used.

## Verification

- Unit-test token precedence, persistence, and clearing.
- Contract-test that JSON requests and photo uploads use the selected user token.
- Keep device-token contract tests unchanged.
- Run all API/Web tests and production builds.
- Use the browser to verify the seeded demo still loads and protected images render.

