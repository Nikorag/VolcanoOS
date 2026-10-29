# API gaps for Volcano OS

Volcano OS draws every non-game page natively from JSON. Web views are used for
**games only**. These are the endpoints it still needs. All are bearer-authenticated
like the rest of `/api`, JSON in/out, errors as `{ "error": string }`.

## 1. Store (blocks the native Store page)

The store is currently CMS pages + server-rendered React; there is no JSON catalogue,
and purchase requires a PayPal popup in a browser.

### `GET /api/store/products`
Published listings only (drafts never appear). Includes ownership for the caller.
```json
[{
  "slug": "carve-the-fate",         // product slug (also the game slug)
  "gameSlug": "carve-the-fate",
  "title": "Carve The Fate",
  "tagline": "…",
  "tags": "ACTION · ROGUELIKE",
  "pricePence": 999,                 // null = not for sale
  "salePct": 20,                     // 0 if none
  "salePricePence": 799,             // effective price; 0 = free
  "flag": null,                      // "soon" | "new" | "sale" | null
  "logoUrl": "…", "heroUrl": "…",
  "owned": false,
  "platforms": ["darwin","windows"]  // optional: which builds exist
}]
```

### `GET /api/store/products/[slug]`
Same fields plus `description` (plain text or constrained Markdown), `screenshots: string[]`,
`version`, `sizeBytes`. 404 for drafts / deleted games.

### Purchase, one of:
- **Free titles:** `POST /api/store/claim { slug }` already exists. Nothing needed.
- **Paid titles (needs a new flow):** the launcher can't host PayPal's approval UI.
  Proposed device-style handoff:
  - `POST /api/store/checkout/link { slugs: string[] }` -> `{ orderId, approveUrl, expiresAt }`.
    `approveUrl` is the PayPal approval URL (or a short Zonto page that wraps it); the launcher opens it
    in the system browser and shows the order total + "waiting for payment".
  - `GET /api/store/orders/[orderId]` -> `{ status: "pending" | "paid" | "failed" | "expired" }`.
    The launcher polls; on `paid` it refreshes the library. The capture step that
    `/api/store/checkout/capture` does today must run server-side on PayPal's return redirect
    (or webhook) rather than from the browser.

## 2. Nice to have

- **Player search / profiles:** `GET /api/players/[gamertag]` (public stats, level, mutual games)
  and `GET /api/players?q=` so "Add friend" can autocomplete instead of blind-typing a gamertag.
- **Chat unread counts:** `GET /api/messages/unread` -> `[{ userId, count }]` for badges on the friends list.
  Today the launcher has to open each conversation to know. (The `/api/ws` socket would also work.)
- **Party / lobby invites:** the README documents `GET /api/party` and `POST /api/party/[id]/join`
  but no such route exists under `src/app/(site)/api`. `GET /api/invites` does exist and could feed
  an "invites" row once confirmed.
- **Friend removal:** there's accept/decline for requests but no `DELETE /api/friends/[id]` to unfriend.
- **Achievements per game with game art:** `/api/achievements` returns `game {slug, name}` only;
  adding `logoUrl` would let the page show artwork without cross-referencing the library.

## Already covered natively (no server work)
Achievements (`/api/achievements`), Friends/requests/activity (`/api/friends`, `/api/friends/requests`,
`/api/activity`), Chat (`/api/messages/[userId]`), Profile + privacy (`/api/profile`, `/api/settings`),
Devices (`/api/devices`), Library, install, launch, presence.
