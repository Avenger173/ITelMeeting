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

3. Server implementation and historical deployment
- Go signaling service directory: `golangSignaling`
- Historical signaling service name: `smartmeet-go-signal`
- Media server: ZLMediaKit
- Historical database: MySQL
- The old cloud instance may have been released; current availability is unverified. Local Linux services have not yet been restored.
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

6. Existing AI and media enhancements
- Independent assistant dialog with general/project modes, LLM integration and short-term conversation history
- ONNX portrait segmentation with blur, solid-color and image virtual backgrounds
- YOLO local preview overlays and a UI switch; detection overlays are not published to the meeting
- RNNoise integration remains local uncommitted work and needs separate verification

7. Planned meeting upgrade (not implemented)
- Target scenario: small research-group meetings.
- First restore local Linux services and migrate both publishing and subscribing to WebRTC + SFU; the running media implementation is still RTMP.
- Add persistent shared meeting chat and per-participant ASR transcripts with authenticated identity, meeting-session ID, timestamps and sequence cursors.
- Host-enabled shared transcription runs independently of any individual participant's client. Authorized late joiners/reconnected members can retrieve earlier saved public records; uncaptured gaps cannot be recovered automatically.
- Add a private text-based meeting Agent that queries authorized records through validated tools, provides source-linked answers and on-demand summaries, and produces editable/exportable minutes.
- Private questions, answers and conversation history are isolated by user, meeting and thread; they are not broadcast or inserted into shared minutes.
- AI speech synthesis, autonomous speaking and stopping speech playback are outside the current delivery scope. OpenGL and the module marketplace are deferred.
- These are roadmap items, not existing tools, endpoints or database tables. Do not claim that the current assistant can already read meeting history or invoke these tools.
