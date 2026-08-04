# REX Cloud Sync

## Architecture & Design Specification v1.0

---

## 1. Overview & Purpose

Cloud Sync enables users to seamlessly synchronise their entire REX environment—profiles, settings, wallpapers, widget layouts, themes, and marketplace subscriptions—across multiple Windows devices. It is designed to be a **privacy‑first, encrypted, and user‑controlled** service that makes the REX desktop environment truly portable.

### Goals
- **Zero‑touch sync**: after signing in on a new device, the user’s desktop experience is restored within minutes.
- **Selective sync**: users control which categories (wallpapers, settings, profiles, widgets) are synced.
- **Conflict resolution**: automatic merging with manual override for conflicting changes.
- **Bandwidth‑aware**: large assets (wallpaper files) are synced only on Wi‑Fi by default; preview thumbnails sync on any connection.
- **End‑to‑end encryption**: user data is encrypted before it leaves the device; REX servers cannot read the content.

### Non‑Goals
- Real‑time collaborative editing (not needed).
- Syncing running process state (only configuration).
- Replacing external backup solutions; Cloud Sync is additive, not a full backup.

---

## 2. Architecture

```
┌──────────────┐       ┌─────────────────┐       ┌──────────────┐
│  Device A    │ ←───→ │  REX Cloud API  │ ←───→ │  Device B    │
│  (REX Client)│       │  (Storage +     │       │  (REX Client)│
│              │       │   Auth)          │       │              │
└──────────────┘       └─────────────────┘       └──────────────┘
```

### Components
- **Sync Client** – embedded in REX, runs as a background service within the app.
- **Sync Engine** – manages local change tracking, push/pull queues, conflict resolution.
- **Local Store** – SQLite database holding sync metadata (change timestamps, version vectors).
- **Cloud Service** – a REST API (or Azure/AWS backend) providing authenticated storage and basic CRUD for sync data.
- **Encryption Layer** – AES‑256‑GCM encryption of all sync payloads before transmission; keys derived from user password + device TPM where available.

### Technology Stack
- **Transport**: HTTPS with mutual TLS (certificate pinning).
- **Storage**: Cloud object storage (S3, Azure Blob) for assets; metadata in a simple key‑value store.
- **Messaging**: WebSocket for push notifications (optional, for immediate sync).

---

## 3. Data Model

Sync data is organised into **collections**, each representing a category of user data.

| Collection          | Content                                          | Typical Size |
|---------------------|--------------------------------------------------|--------------|
| `profiles`          | Profile definitions (JSON)                        | < 10 KB      |
| `settings`          | All key‑value settings                           | < 100 KB     |
| `wallpaper_library` | Wallpaper metadata (not files – files separate)  | < 500 KB     |
| `widget_instances`  | Widget positions, settings                       | < 50 KB      |
| `themes`            | Custom theme tokens                              | < 20 KB      |
| `marketplace`       | List of installed packages, subscriptions        | < 10 KB      |
| `wallpaper_files`   | Actual image/video files                         | 1 MB – 2 GB  |
| `widget_packages`   | Widget HTML/DLL files                            | 1 – 50 MB    |

### Sync Object Schema (generic)
```json
{
  "collection": "settings",
  "key": "desktop.theme.dark_mode",
  "value": "true",
  "version": 12,
  "lastModified": "2025-01-15T10:30:00Z",
  "deviceId": "device-uuid"
}
```

Each device maintains a **version vector** (`deviceId → maxVersion`) to know what it has already synced.

---

## 4. Sync Flow

### 4.1 Initial Sync
1. User signs in to REX account on device A.
2. Client generates a local encryption key (derived from password + salt).
3. Client uploads all local collections to cloud, encrypted with the key.
4. On device B, after sign‑in, client downloads the latest snapshot.
5. Client merges: for each collection key, if both sides changed, apply conflict resolution; otherwise take the newer version.

### 4.2 Incremental Sync
- Every 5 minutes (configurable), the client:
  - Pushes local changes (since last sync) to the cloud.
  - Pulls remote changes from the cloud.
- Large file uploads use chunked transfer and resumable uploads.
- If a change is detected while the app is in use, the sync is deferred until the user is idle.

### 4.3 Selective Sync
From the Sync settings page, users can toggle collections on/off. When a collection is disabled, its data is neither uploaded nor downloaded. It remains local only.

---

## 5. Conflict Resolution

When the same key is changed on two devices before either syncs, a conflict occurs. REX uses a **last‑writer‑wins (LWW)** strategy with automatic backup of the losing version.

- Each object has a `lastModified` timestamp (UTC).
- The version with the later timestamp wins.
- The losing value is saved in a local `conflicts.json` and shown in the Sync settings page for manual resolution.
- For complex types like profiles, a deeper merge can be attempted (e.g., merging widget lists by union).

Users can always restore a conflict version via the UI.

---

## 6. Security & Privacy

### 6.1 End‑to‑End Encryption
- All sync data is encrypted on the client using a key derived from the user’s password + a random salt (PBKDF2‑HMAC‑SHA256).
- The key is never sent to the server.
- Password changes re‑encrypt the data with the new key; old keys are securely erased.
- File attachments are encrypted with a separate symmetric file key, which is itself encrypted with the user key and stored in the metadata.

### 6.2 Authentication
- REX account system (email + password) with optional 2FA.
- OAuth2 token flow; access tokens have a short lifetime, refresh tokens are stored securely in Windows Credential Manager.
- Devices can be remotely deauthorised from the account web portal.

### 6.3 Server‑Side Security
- The cloud service stores only encrypted blobs and opaque metadata.
- Server has no access to plaintext content.
- Infrastructure follows SOC 2 / ISO 27001 best practices (if self‑hosted, these are recommendations).

---

## 7. Performance & Offline Support

### 7.1 Bandwidth Management
- Large files (wallpapers) are queued and synced only when on Wi‑Fi (configurable).
- Thumbnails and metadata are synced immediately on any connection.
- Sync uses delta‑encoding where possible (e.g., only changed key‑value pairs).

### 7.2 Offline Behaviour
- REX works fully offline.
- Changes made offline are queued locally and synced when connectivity is restored.
- Conflict windows increase with offline time; LWW handles this gracefully.

### 7.3 Resource Limits
- Total cloud storage per user: 5 GB free, expandable via subscription.
- Maximum individual file: 2 GB.
- Version history retained for 30 days.

---

## 8. Integration Points

### 8.1 Profiles
Syncs entire profile snapshots. Applying a profile on a new device restores all associated settings, widgets, and wallpaper references (files synced separately if missing).

### 8.2 Settings
Full key‑value store is synced. Filtered by category for selective sync.

### 8.3 Wallpaper Library
Metadata is synced; actual files are synced lazily. On a new device, if a wallpaper file is missing, a placeholder thumbnail is shown, and the file is downloaded on demand (or in background).

### 8.4 Widget Platform
Widget instances (positions, settings) are synced. The widget packages themselves are re‑downloaded from Marketplace if not present locally.

### 8.5 Marketplace
Subscription list (purchased items) is synced to maintain access across devices.

---

## 9. User Experience

### 9.1 Settings Page
A new section in Settings > Cloud Sync:
- Sign in / Sign up
- Account details (email, storage used)
- Selective sync toggles for each collection
- Sync status (last sync time, pending changes)
- Conflict viewer (list of conflicts with option to choose a version)
- "Sync Now" button
- Deauthorise device / Sign out

### 9.2 Status Indicator
A small cloud icon in the status bar shows sync state:
- Grey: signed out
- Blue: syncing
- Green: up‑to‑date
- Yellow: conflict pending
- Red: error

### 9.3 First‑Run Experience
After sign‑in on a new device, a wizard offers to restore from cloud: "We found your REX setup. Restore it now?"

---

## 10. Implementation Phases

### Phase 1 — Core Sync Engine
- Local SQLite change tracking
- Version vector management
- Push/pull logic with mock cloud endpoint
- Encryption/decryption module
- Settings page UI (sign‑in mock)

### Phase 2 — Cloud Service
- Production REST API (Azure Functions or similar)
- Authentication service
- Blob storage for assets
- WebSocket for push notifications

### Phase 3 — File Sync
- Chunked upload/download with resume
- Large file queue management
- Wi‑Fi detection

### Phase 4 — Conflict UI & Polish
- Conflict viewer
- Restore from cloud wizard
- Selective sync UI
- Status bar indicator

---

## 11. API Design (Cloud Service)

### Endpoints
- `POST /auth/signup` – create account
- `POST /auth/login` – obtain tokens
- `GET /sync/collections` – list collection versions
- `POST /sync/push` – upload encrypted sync objects
- `POST /sync/pull` – download encrypted sync objects since version
- `PUT /files/{id}` – upload file chunk
- `GET /files/{id}` – download file
- `DELETE /device/{id}` – deauthorise device

All endpoints are authenticated via Bearer token.

---

## 12. Edge Cases & Failure Recovery

| Scenario                          | Behaviour |
|-----------------------------------|-----------|
| Network loss during sync          | Queue retry with exponential backoff |
| Server returns 409 Conflict       | Trigger full re‑sync for that collection |
| Local storage corruption          | Detect via checksum; restore from cloud |
| Encryption key forgotten          | Account recovery via email (key is not stored server‑side, so data is lost – user must reset and re‑upload) |
| File too large for free tier      | Warn user, offer storage upgrade |
| Device deauthorised remotely      | Wipe local sync data, notify user |

---

*Cloud Sync transforms REX from a single‑device customisation tool into a platform‑independent desktop environment. This specification provides the foundation for implementation, ensuring that user data remains secure, portable, and always up‑to‑date.*
