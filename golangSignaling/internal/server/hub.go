package server

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"log"
	"net/http"
	"sort"
	"strings"
	"sync"
	"time"

	"golangsignaling/internal/storage"

	"github.com/gorilla/websocket"
)

type Hub struct {
	store *storage.Store

	upgrader websocket.Upgrader

	mu                 sync.RWMutex
	clients            map[*Client]struct{}
	roomHostStream     map[string]string
	roomCoHosts        map[string]map[string]struct{}
	roomWhiteboardLock map[string]bool
}

func NewHub(store *storage.Store) *Hub {
	return &Hub{
		store: store,
		upgrader: websocket.Upgrader{
			CheckOrigin: func(r *http.Request) bool { return true },
		},
		clients:            make(map[*Client]struct{}),
		roomHostStream:     make(map[string]string),
		roomCoHosts:        make(map[string]map[string]struct{}),
		roomWhiteboardLock: make(map[string]bool),
	}
}

func (h *Hub) ServeWS(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" && r.URL.Path != "/ws" {
		http.NotFound(w, r)
		return
	}

	conn, err := h.upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("upgrade failed: %v", err)
		return
	}

	c := &Client{
		conn: conn,
		hub:  h,
	}

	h.mu.Lock()
	h.clients[c] = struct{}{}
	h.mu.Unlock()

	log.Printf("client connected")

	c.readLoop()
	h.onSocketClosed(c)
}

func (c *Client) readLoop() {
	c.conn.SetReadLimit(1024 * 1024)

	for {
		_, data, err := c.conn.ReadMessage()
		if err != nil {
			return
		}
		if err := c.hub.handleRawMessage(c, data); err != nil {
			log.Printf("handle message failed: %v", err)
		}
	}
}

func (c *Client) sendJSON(v any) error {
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}

	c.writeMu.Lock()
	defer c.writeMu.Unlock()

	_ = c.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
	return c.conn.WriteMessage(websocket.TextMessage, data)
}

func (h *Hub) onSocketClosed(c *Client) {
	room := h.detachClientRoom(c, "disconnect")

	h.mu.Lock()
	delete(h.clients, c)
	h.mu.Unlock()

	_ = c.conn.Close()

	if room != "" {
		h.broadcastMembers(room)
	}

	log.Printf("client disconnected")
}

func (h *Hub) handleRawMessage(c *Client, raw []byte) error {
	var base baseMessage
	if err := json.Unmarshal(raw, &base); err != nil {
		return err
	}

	switch base.Type {
	case "auth_register":
		var msg authRegisterMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return err
		}
		h.handleAuthRegister(c, msg)
	case "auth_login":
		var msg authLoginMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return err
		}
		h.handleAuthLogin(c, msg)
	case "join":
		var msg joinMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return err
		}
		h.handleJoin(c, msg)
	case "leave":
		h.handleLeave(c)
	case "update":
		var msg updateMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return err
		}
		h.handleUpdate(c, msg)
	case "chat":
		var msg chatMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return err
		}
		h.handleChat(c, msg)
	case "wb":
		var msg whiteboardMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return err
		}
		h.handleWhiteboard(c, raw, msg)
	case "cmd":
		var msg cmdMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return err
		}
		h.handleCmd(c, msg)
	case "ping":
		_ = c.sendJSON(pongMessage{Type: "pong"})
	default:
		log.Printf("unsupported type: %s", base.Type)
	}

	return nil
}

func (h *Hub) handleAuthRegister(c *Client, msg authRegisterMessage) {
	user := strings.TrimSpace(msg.User)
	pwd := msg.Password

	if user == "" || len(pwd) < 6 {
		_ = c.sendJSON(authFailMessage{
			Type: "auth_fail",
			Code: "bad_args",
			Msg:  "账号为空或密码过短",
		})
		return
	}

	err := h.store.RegisterUser(user, hashPassword(user, pwd), "member")
	if err != nil {
		lower := strings.ToLower(err.Error())
		if strings.Contains(lower, "duplicate") || strings.Contains(lower, "unique") {
			_ = c.sendJSON(authFailMessage{
				Type: "auth_fail",
				Code: "exists",
				Msg:  "账号已存在",
			})
			return
		}
		_ = c.sendJSON(authFailMessage{
			Type: "auth_fail",
			Code: "db_error",
			Msg:  err.Error(),
		})
		return
	}

	_ = c.sendJSON(authRegisteredMessage{
		Type: "auth_registered",
		User: user,
	})
}

func (h *Hub) handleAuthLogin(c *Client, msg authLoginMessage) {
	user := strings.TrimSpace(msg.User)
	pwd := msg.Password

	if user == "" || pwd == "" {
		_ = c.sendJSON(authFailMessage{
			Type: "auth_fail",
			Code: "bad_args",
			Msg:  "账号或密码为空",
		})
		return
	}

	row, found, err := h.store.FindUser(user)
	if err != nil {
		_ = c.sendJSON(authFailMessage{
			Type: "auth_fail",
			Code: "db_error",
			Msg:  err.Error(),
		})
		return
	}
	if !found {
		_ = c.sendJSON(authFailMessage{
			Type: "auth_fail",
			Code: "no_user",
			Msg:  "账号不存在",
		})
		return
	}
	if row.PasswordHash != hashPassword(user, pwd) {
		_ = c.sendJSON(authFailMessage{
			Type: "auth_fail",
			Code: "bad_password",
			Msg:  "密码错误",
		})
		return
	}

	c.auth = AuthState{
		Authed: true,
		User:   row.User,
		Role:   row.Role,
	}

	_ = c.sendJSON(authOKMessage{
		Type: "auth_ok",
		User: row.User,
		Role: row.Role,
	})
}

func (h *Hub) handleJoin(c *Client, msg joinMessage) {
	if !c.auth.Authed {
		_ = c.sendJSON(authRequiredMessage{
			Type: "auth_required",
			Msg:  "请先登录",
		})
		return
	}

	room := strings.TrimSpace(msg.Room)
	stream := strings.TrimSpace(msg.Stream)
	if room == "" || stream == "" {
		return
	}

	info := &ClientInfo{
		Room:       room,
		User:       c.auth.User,
		Stream:     stream,
		Audio:      boolOr(msg.Audio, true),
		Video:      boolOr(msg.Video, true),
		Publishing: boolOr(msg.Pub, false),
		Share:      boolOr(msg.Share, false),
	}

	var duplicates []*Client

	h.mu.Lock()
	for peer := range h.clients {
		if peer == c || peer.info == nil {
			continue
		}
		if peer.info.Room == room && peer.info.Stream == stream {
			duplicates = append(duplicates, peer)
			peer.info = nil
		}
	}
	c.info = info
	if _, ok := h.roomHostStream[room]; !ok {
		h.roomHostStream[room] = stream
	}
	h.mu.Unlock()

	for _, peer := range duplicates {
		_ = peer.conn.Close()
	}

	_ = h.store.AppendMeetingEvent(
		room,
		"join",
		info.User,
		info.Stream,
		"",
		map[string]any{
			"audio": info.Audio,
			"video": info.Video,
			"pub":   info.Publishing,
			"share": info.Share,
		},
	)

	h.broadcastMembers(room)
}

func (h *Hub) handleLeave(c *Client) {
	room := h.detachClientRoom(c, "client_leave")
	if room != "" {
		h.broadcastMembers(room)
	}
}

func (h *Hub) handleUpdate(c *Client, msg updateMessage) {
	h.mu.Lock()
	if c.info == nil {
		h.mu.Unlock()
		return
	}

	info := c.info
	room := info.Room
	actorUser := info.User
	actorStream := info.Stream

	oldAudio := info.Audio
	oldVideo := info.Video
	oldPub := info.Publishing
	oldShare := info.Share

	if msg.Audio != nil {
		info.Audio = *msg.Audio
	}
	if msg.Video != nil {
		info.Video = *msg.Video
	}
	if msg.Pub != nil {
		info.Publishing = *msg.Pub
	}
	if msg.Share != nil {
		info.Share = *msg.Share
	}

	newAudio := info.Audio
	newVideo := info.Video
	newPub := info.Publishing
	newShare := info.Share
	h.mu.Unlock()

	if oldPub != newPub {
		eventType := "publish_stop"
		if newPub {
			eventType = "publish_start"
		}
		_ = h.store.AppendMeetingEvent(room, eventType, actorUser, actorStream, "", nil)
	}
	if oldShare != newShare {
		eventType := "share_off"
		if newShare {
			eventType = "share_on"
		}
		_ = h.store.AppendMeetingEvent(room, eventType, actorUser, actorStream, "", nil)
	}
	if oldAudio != newAudio {
		eventType := "self_audio_off"
		if newAudio {
			eventType = "self_audio_on"
		}
		_ = h.store.AppendMeetingEvent(room, eventType, actorUser, actorStream, "", nil)
	}
	if oldVideo != newVideo {
		eventType := "self_video_off"
		if newVideo {
			eventType = "self_video_on"
		}
		_ = h.store.AppendMeetingEvent(room, eventType, actorUser, actorStream, "", nil)
	}

	h.broadcastMembers(room)
}

func (h *Hub) handleChat(c *Client, msg chatMessage) {
	h.mu.RLock()
	info := c.info
	h.mu.RUnlock()

	if info == nil {
		return
	}

	room := strings.TrimSpace(msg.Room)
	if room == "" || room != info.Room {
		return
	}

	content := strings.TrimSpace(msg.Content)
	if content == "" {
		return
	}
	if len(content) > 500 {
		content = content[:500]
	}

	msgID := strings.TrimSpace(msg.MsgID)
	if msgID == "" {
		msgID = info.Stream + "_" + time.Now().Format("20060102150405.000")
	}

	out := chatBroadcastMessage{
		Type:    "chat",
		Room:    room,
		User:    info.User,
		Stream:  info.Stream,
		Content: content,
		MsgID:   msgID,
		TS:      nowMs(),
		Ver:     1,
	}

	for _, peer := range h.clientsInRoom(room) {
		_ = peer.sendJSON(out)
	}

	log.Printf("[CHAT] room=%s from=%s len=%d", room, info.Stream, len(content))
}

func (h *Hub) handleWhiteboard(c *Client, raw []byte, msg whiteboardMessage) {
	h.mu.Lock()
	if c.info == nil {
		h.mu.Unlock()
		return
	}

	senderInfo := *c.info
	room := strings.TrimSpace(msg.Room)
	if room == "" || room != senderInfo.Room {
		h.mu.Unlock()
		return
	}

	op := strings.TrimSpace(msg.Op)
	hostStream := h.roomHostStream[room]
	senderIsHost := senderInfo.Stream == hostStream
	_, senderIsCohost := h.roomCoHosts[room][senderInfo.Stream]
	locked := h.roomWhiteboardLock[room]

	if op == "lock" || op == "unlock" {
		if !(senderIsHost || senderIsCohost) {
			h.mu.Unlock()
			return
		}
		h.roomWhiteboardLock[room] = (op == "lock")
	}

	if (op == "draw" || op == "undo") && locked && !(senderIsHost || senderIsCohost) {
		h.mu.Unlock()
		return
	}
	if op == "clear" && !(senderIsHost || senderIsCohost) {
		h.mu.Unlock()
		return
	}

	h.mu.Unlock()

	var out map[string]any
	if err := json.Unmarshal(raw, &out); err != nil {
		return
	}

	out["room"] = room
	out["stream"] = senderInfo.Stream
	if _, ok := out["ts"]; !ok {
		out["ts"] = nowMs()
	}
	if _, ok := out["msg_id"]; !ok {
		out["msg_id"] = "wb_" + senderInfo.Stream + "_" + time.Now().Format("20060102150405.000")
	}

	if op == "lock" || op == "unlock" {
		h.broadcastMembers(room)
	}

	for _, peer := range h.clientsInRoom(room) {
		_ = peer.sendJSON(out)
	}
}

func (h *Hub) handleCmd(c *Client, msg cmdMessage) {
	room := strings.TrimSpace(msg.Room)
	to := strings.TrimSpace(msg.To)
	action := strings.TrimSpace(msg.Action)
	if room == "" || to == "" || action == "" {
		return
	}

	h.mu.Lock()
	if c.info == nil || c.info.Room != room {
		h.mu.Unlock()
		return
	}

	senderInfo := *c.info
	fromStream := senderInfo.Stream
	hostStream := h.roomHostStream[room]
	if hostStream == "" {
		h.mu.Unlock()
		return
	}

	senderIsHost := fromStream == hostStream
	_, senderIsCohost := h.roomCoHosts[room][fromStream]
	if !(senderIsHost || senderIsCohost) {
		h.mu.Unlock()
		return
	}

	target := h.findClientByRoomStreamLocked(room, to)
	if target == nil || target.info == nil {
		h.mu.Unlock()
		return
	}

	targetIsHost := to == hostStream
	_, targetIsCohost := h.roomCoHosts[room][to]

	log.Printf("[CMD] room=%s from=%s to=%s action=%s", room, fromStream, to, action)

	ctrlAction := action
	needBroadcast := false
	needCloseTarget := false
	var kickRoom string

	switch action {
	case "kick":
		if !senderIsHost && (targetIsHost || targetIsCohost) {
			h.mu.Unlock()
			return
		}
		needCloseTarget = true
		needBroadcast = true
		kickRoom = h.removeClientFromRoomLocked(target)
		delete(h.roomCoHosts[room], to)
	case "set_host":
		if !senderIsHost || targetIsHost {
			h.mu.Unlock()
			return
		}
		h.roomHostStream[room] = to
		delete(h.roomCoHosts[room], to)
		needBroadcast = true
	case "set_cohost":
		if !senderIsHost || targetIsHost {
			h.mu.Unlock()
			return
		}
		if h.roomCoHosts[room] == nil {
			h.roomCoHosts[room] = make(map[string]struct{})
		}
		h.roomCoHosts[room][to] = struct{}{}
		needBroadcast = true
	case "unset_cohost":
		if !senderIsHost {
			h.mu.Unlock()
			return
		}
		delete(h.roomCoHosts[room], to)
		needBroadcast = true
	case "mute_audio":
		if !senderIsHost && (targetIsHost || targetIsCohost) {
			h.mu.Unlock()
			return
		}
		target.info.Audio = false
		needBroadcast = true
	case "unmute_audio", "restore_audio", "audio_on":
		if !senderIsHost && (targetIsHost || targetIsCohost) {
			h.mu.Unlock()
			return
		}
		target.info.Audio = true
		ctrlAction = "unmute_audio"
		needBroadcast = true
	case "mute_video":
		if !senderIsHost && (targetIsHost || targetIsCohost) {
			h.mu.Unlock()
			return
		}
		target.info.Video = false
		needBroadcast = true
	case "unmute_video", "restore_video", "video_on":
		if !senderIsHost && (targetIsHost || targetIsCohost) {
			h.mu.Unlock()
			return
		}
		target.info.Video = true
		ctrlAction = "unmute_video"
		needBroadcast = true
	case "stop_share":
		if !senderIsHost && (targetIsHost || targetIsCohost) {
			h.mu.Unlock()
			return
		}
		target.info.Share = false
		needBroadcast = true
	default:
		h.mu.Unlock()
		log.Printf("[CMD] unsupported action: %s", action)
		return
	}

	targetConn := target
	h.mu.Unlock()

	_ = targetConn.sendJSON(ctrlMessage{
		Type:   "ctrl",
		Action: ctrlAction,
		To:     to,
		By:     fromStream,
	})

	_ = h.store.AppendMeetingEvent(room, ctrlAction, senderInfo.User, fromStream, to, nil)

	if needCloseTarget {
		_ = targetConn.conn.Close()
	}

	if needBroadcast {
		if kickRoom != "" {
			h.broadcastMembers(kickRoom)
		} else {
			h.broadcastMembers(room)
		}
	}
}

func (h *Hub) clientsInRoom(room string) []*Client {
	h.mu.RLock()
	defer h.mu.RUnlock()

	out := make([]*Client, 0)
	for c := range h.clients {
		if c.info != nil && c.info.Room == room {
			out = append(out, c)
		}
	}
	return out
}

func (h *Hub) findClientByRoomStreamLocked(room, stream string) *Client {
	for c := range h.clients {
		if c.info != nil && c.info.Room == room && c.info.Stream == stream {
			return c
		}
	}
	return nil
}

func (h *Hub) removeClientFromRoomLocked(c *Client) string {
	if c.info == nil {
		return ""
	}

	leaving := *c.info
	c.info = nil

	if cohosts, ok := h.roomCoHosts[leaving.Room]; ok {
		delete(cohosts, leaving.Stream)
		if len(cohosts) == 0 {
			delete(h.roomCoHosts, leaving.Room)
		}
	}

	if h.roomHostStream[leaving.Room] == leaving.Stream {
		delete(h.roomHostStream, leaving.Room)
	}

	return leaving.Room
}

func (h *Hub) detachClientRoomLocked(c *Client, reason string) string {
	if c.info == nil {
		return ""
	}

	leaving := *c.info
	room := h.removeClientFromRoomLocked(c)

	_ = h.store.AppendMeetingEvent(
		leaving.Room,
		"leave",
		leaving.User,
		leaving.Stream,
		"",
		map[string]any{"reason": reason},
	)

	return room
}

func (h *Hub) detachClientRoom(c *Client, reason string) string {
	h.mu.Lock()
	room := h.detachClientRoomLocked(c, reason)
	h.mu.Unlock()
	return room
}

func (h *Hub) broadcastMembers(room string) {
	h.mu.Lock()

	roomClients := make([]*Client, 0)
	roomInfos := make([]ClientInfo, 0)

	for c := range h.clients {
		if c.info == nil || c.info.Room != room {
			continue
		}
		roomClients = append(roomClients, c)
		roomInfos = append(roomInfos, *c.info)
	}

	if len(roomInfos) == 0 {
		delete(h.roomHostStream, room)
		delete(h.roomCoHosts, room)
		delete(h.roomWhiteboardLock, room)
		h.mu.Unlock()
		return
	}

	live := make(map[string]struct{}, len(roomInfos))
	streams := make([]string, 0, len(roomInfos))
	for _, info := range roomInfos {
		live[info.Stream] = struct{}{}
		streams = append(streams, info.Stream)
	}

	hostStream := h.roomHostStream[room]
	if _, ok := live[hostStream]; !ok {
		sort.Strings(streams)
		hostStream = streams[0]
		h.roomHostStream[room] = hostStream
	}

	cohosts := h.roomCoHosts[room]
	if cohosts == nil {
		cohosts = make(map[string]struct{})
	}
	for stream := range cohosts {
		if _, ok := live[stream]; !ok || stream == hostStream {
			delete(cohosts, stream)
		}
	}
	if len(cohosts) == 0 {
		delete(h.roomCoHosts, room)
	} else {
		h.roomCoHosts[room] = cohosts
	}

	members := make([]memberSnapshot, 0, len(roomInfos))
	for _, info := range roomInfos {
		role := "member"
		if info.Stream == hostStream {
			role = "host"
		} else if _, ok := cohosts[info.Stream]; ok {
			role = "cohost"
		}
		members = append(members, memberSnapshot{
			User:   info.User,
			Stream: info.Stream,
			Audio:  info.Audio,
			Video:  info.Video,
			Pub:    info.Publishing,
			Share:  info.Share,
			Role:   role,
		})
	}

	lockState := h.roomWhiteboardLock[room]
	h.mu.Unlock()

	msg := membersMessage{
		Type:    "members",
		Room:    room,
		Members: members,
		WBLock:  lockState,
	}

	for _, c := range roomClients {
		_ = c.sendJSON(msg)
	}
}

func hashPassword(user, pwd string) string {
	raw := user + "|" + pwd + "|SmartMeet_v1"
	sum := sha256.Sum256([]byte(raw))
	return hex.EncodeToString(sum[:])
}
