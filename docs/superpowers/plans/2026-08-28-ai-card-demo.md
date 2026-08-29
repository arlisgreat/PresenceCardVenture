# AI 小卡 Demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a locally runnable, testable AI 小卡 community demo covering photo creation, social feed, messaging, AI composite jobs, and device simulation without real hardware.

**Architecture:** Keep the existing React/Vite web app and Fastify API as separate processes. Implement a demo repository with seeded in-memory data and local uploads behind interfaces, then keep the public `/v1` device contract stable so storage, algorithm, and hardware implementations can be swapped later.

**Tech Stack:** React 18, Vite 5, TypeScript, Fastify 4, Prisma schema as the production model, Node test runner with Fastify `inject`, local filesystem uploads, CSS modules-free component styles.

## Global Constraints

- Work only in `/Users/qingflow/Downloads/PresenceCardVenture-main` on `codex/fullstack`.
- Do not require a real ESP32, production OSS bucket, external AI key, or network access for the demo.
- Preserve the device API semantics in `docs/02-device-api-v1.md` and `docs/03-device-api.openapi.yaml`.
- Do not commit `.env`, uploads, generated files, or user-provided images.
- Keep original and processed image ownership/deletion explicit.
- Every task ends with a focused verification command and a small Git commit.

### Task 1: Make API and Web runnable in demo mode

**Files:**
- Modify: `server/api/src/index.ts`
- Modify: `server/api/package.json`
- Modify: `server/web/src/App.tsx`
- Create: `server/api/src/app.ts`
- Create: `server/api/src/demo-store.ts`

**Interfaces:** `buildApp()` returns a Fastify instance for tests and runtime; `DemoStore` exposes seeded users, photos, messages, reactions and jobs.

- [ ] Add an API factory that does not listen during tests.
- [ ] Add an in-memory store with deterministic demo users and sample records.
- [ ] Add a web API client base path using the existing Vite `/v1` proxy.
- [ ] Run `npm --prefix server/api run build` and `npm --prefix server/web run build`.
- [ ] Commit `feat: add demo api foundation`.

### Task 2: Implement photo upload, processing and feed

**Files:**
- Create: `server/api/src/routes/photos.ts`
- Create: `server/api/src/photo-store.ts`
- Create: `server/api/test/photos.test.ts`
- Modify: `server/api/src/app.ts`
- Modify: `server/api/src/demo-store.ts`

**Interfaces:** `POST /v1/photos` accepts JPEG bytes and metadata headers; `GET /v1/feed` returns feed items; `GET /v1/photos/:id/image` streams the stored file; `DELETE /v1/photos/:id` deletes owned assets.

- [ ] Test valid JPEG upload, 1MB rejection, idempotent retry and unauthorized deletion.
- [ ] Add a JPEG content-type parser with a 1MB body limit and local file adapter.
- [ ] Store original and processed variants with filter/beauty metadata.
- [ ] Return feed items with author, filter, image URL, reactions and ownership.
- [ ] Run API photo tests and build.
- [ ] Commit `feat: add photo upload and feed api`.

### Task 3: Add social interactions and AI job API

**Files:**
- Create: `server/api/src/routes/social.ts`
- Create: `server/api/src/routes/ai.ts`
- Create: `server/api/test/social-ai.test.ts`
- Modify: `server/api/src/app.ts`
- Modify: `server/api/src/demo-store.ts`

**Interfaces:** reactions, friends, conversations/messages, `POST /v1/ai/jobs`, `GET /v1/ai/jobs/:id`, and result deletion all use JSON and deterministic error responses.

- [ ] Test friend visibility, reaction toggling, message creation, AI authorization and failed jobs.
- [ ] Implement seeded friend list and conversation endpoints.
- [ ] Implement an asynchronous local AI adapter with queued/processing/completed states.
- [ ] Add explicit result deletion and ownership checks.
- [ ] Run all API tests and build.
- [ ] Commit `feat: add social and ai demo api`.

### Task 4: Build the Web application shell and create studio

**Files:**
- Modify: `server/web/src/App.tsx`
- Create: `server/web/src/api.ts`
- Create: `server/web/src/styles.css`
- Create: `server/web/src/components/CreateStudio.tsx`
- Create: `server/web/src/components/PhotoCard.tsx`

**Interfaces:** components receive typed props and call `api.ts`; the App shell owns the active view and demo session.

- [ ] Add a mobile-first shell with Feed, Create, Messages, AI Studio and Device Lab views.
- [ ] Build image selection, filter/beauty controls, preview and upload states.
- [ ] Render original/processed relationship and actionable errors.
- [ ] Run the Vite build and check no TypeScript errors.
- [ ] Commit `feat: build demo web shell and create studio`.

### Task 5: Implement feed, messages, AI studio and device lab views

**Files:**
- Create: `server/web/src/components/FeedView.tsx`
- Create: `server/web/src/components/MessagesView.tsx`
- Create: `server/web/src/components/AiStudio.tsx`
- Create: `server/web/src/components/DeviceLab.tsx`
- Modify: `server/web/src/App.tsx`
- Modify: `server/web/src/styles.css`

- [ ] Connect feed cards to reaction and delete endpoints.
- [ ] Connect messages view to friend selection, text and image sending.
- [ ] Connect AI studio to material authorization and job polling.
- [ ] Connect device lab to upload, feed, state and acknowledgement flows.
- [ ] Run the Vite build.
- [ ] Commit `feat: complete demo workflows`.

### Task 6: Verify, document and ship locally

**Files:**
- Modify: `README.md`
- Create: `docs/demo-runbook.md`
- Create: `server/api/test/integration.test.ts`

- [ ] Start API and Web on non-conflicting local ports.
- [ ] Run API integration tests against the actual app factory.
- [ ] Run browser checks for upload, feed, reaction, delete, messages, AI status and device lab.
- [ ] Fix all console errors and failed network requests found during QA.
- [ ] Run API build, Web build, tests, `git diff --check` and inspect status.
- [ ] Commit `docs: add demo runbook and verification evidence`.

