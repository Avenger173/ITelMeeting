SmartMeet confirmed project facts

1. Product positioning
- SmartMeet is a Windows desktop video meeting client built with Qt Widgets.
- The current stable media path is RTMP publish/pull, not WebRTC as the main path.

2. Client-side confirmed capabilities
- Multi-user meeting room and member list sync
- Grid view and double-click focus stream
- Host and co-host controls
- Text chat
- Collaborative whiteboard
- Screen and window sharing
- Local recording and screenshot capture
- Independent AI assistant dialog

3. Server-side confirmed deployment
- Go signaling service directory: `golangSignaling`
- Online signaling service name: `smartmeet-go-signal`
- Media server: ZLMediaKit
- Online database: MySQL
- Historical C++ signaling service `signalTest` is retained mainly as a legacy implementation and migration tool

4. Confirmed database facts
- Confirmed tables in current signaling persistence:
  - `users`
  - `meeting_events`
- Do not invent extra tables such as `chat_messages`, `recordings`, `meetings`, or `whiteboards` unless the user explicitly provides them as real project facts.

5. AI assistant constraints
- The current assistant does not directly read the source code, database contents, or live server state.
- It answers only from:
  - user-provided context
  - configured system prompt
  - short-term conversation history
  - this curated project facts file
- If a project detail is not confirmed here or not provided by the user, the assistant must clearly say it is uncertain instead of fabricating.

6. Current roadmap hints
- Go signaling replacement is already landed.
- OpenGL is planned as the next major rendering upgrade.
- AI virtual background is a planned next-step AI feature.
