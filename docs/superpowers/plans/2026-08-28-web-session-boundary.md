# Web Session Boundary Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route every browser user request through one replaceable session boundary while keeping device credentials explicit and the seeded local demo runnable.

**Architecture:** A focused `user-session.ts` module resolves the selected browser token and provides an authenticated fetch adapter. Existing API helpers and protected image loading consume that adapter; ESP32/device helpers continue to call the token-explicit request helper.

**Tech Stack:** React 18, TypeScript, Vite 5, Node test runner, browser `localStorage`, Fetch API.

## Global Constraints

- `demo-token` is a documented local-demo fallback, not production authentication.
- `VITE_PRESENCE_USER_TOKEN` overrides the fallback when no browser token is saved.
- Device tokens never enter browser user-session storage.
- Service `401` and other HTTP failures must remain visible to the caller.

---

### Task 1: Browser user session module

**Files:**
- Create: `server/web/src/user-session.ts`
- Create: `server/web/src/user-session.test.ts`

**Interfaces:**
- Produces: `getUserToken(): string`, `setUserToken(token: string): void`, `clearUserToken(): void`, and `fetchWithUserSession(input: RequestInfo | URL, init?: RequestInit): Promise<Response>`.

- [x] **Step 1: Write the failing tests**

Test that a saved token wins over the configured/default token, clearing returns to the default, whitespace is ignored, and `fetchWithUserSession` preserves caller headers while adding the bearer credential.

- [x] **Step 2: Run the focused tests and verify RED**

Run: `npm test -- src/user-session.test.ts`

Expected: FAIL because `./user-session.js` does not exist.

- [x] **Step 3: Implement the minimal session module**

Use the storage key `presence.user-token`. Resolve a nonblank saved token first, then `VITE_PRESENCE_USER_TOKEN`, then `demo-token`. Merge request headers with a `Headers` instance and set `Authorization` only from the resolved user token.

- [x] **Step 4: Run the focused tests and verify GREEN**

Run: `npm test -- src/user-session.test.ts`

Expected: all session tests pass with zero failures.

### Task 2: Route web user requests through the boundary

**Files:**
- Modify: `server/web/src/api.ts`
- Modify: `server/web/src/App.tsx`
- Modify: `server/web/src/api.test.ts`
- Modify: `docs/07-frontend-backend-demo-scope.md`

**Interfaces:**
- Consumes: `fetchWithUserSession` from Task 1.
- Preserves: `requestWithToken(path, token, init)` for device endpoints.

- [x] **Step 1: Add failing API contract coverage**

Select `demo-user-2`, call `getCurrentUser` and `uploadPhoto`, and assert both requests carry `Bearer demo-user-2`. Clear the selected token in test cleanup.

- [x] **Step 2: Run the focused API tests and verify RED**

Run: `npm test -- src/api.test.ts`

Expected: FAIL because current helpers still send `Bearer demo-token`.

- [x] **Step 3: Replace user fetch call sites**

Use `fetchWithUserSession` in the JSON request helper, browser photo upload, and `ProtectedImage`. Leave `requestWithToken` and all device-token calls unchanged. Document the new integration boundary and the remaining production auth work.

- [x] **Step 4: Run full verification**

Run in `server/web`: `npm test && npm run build`.

Run in `server/api`: `npm test && npm run build`.

Run at repository root: `git diff --check`.

Expected: zero test failures, both builds exit 0, and no whitespace errors.

- [x] **Step 5: Browser acceptance**

Open `http://localhost:5173/`, confirm the feed loads, protected images render, navigation works at desktop and mobile widths, and the console has no application errors.

- [ ] **Step 6: Commit and synchronize**

Commit message: `refactor: centralize web user session`.

Synchronize `codex/fullstack`, then wait for the matching GitHub Actions run to succeed.
