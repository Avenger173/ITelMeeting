package server

import (
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

type AuthState struct {
	Authed bool
	User   string
	Role   string
}

type ClientInfo struct {
	Room       string
	User       string
	Stream     string
	Audio      bool
	Video      bool
	Publishing bool
	Share      bool
}

type Client struct {
	conn    *websocket.Conn
	hub     *Hub
	writeMu sync.Mutex
	auth    AuthState
	info    *ClientInfo
}

type baseMessage struct {
	Type string `json:"type"`
}

type authRegisterMessage struct {
	Type     string `json:"type"`
	User     string `json:"user"`
	Password string `json:"password"`
}

type authLoginMessage struct {
	Type     string `json:"type"`
	User     string `json:"user"`
	Password string `json:"password"`
}

type joinMessage struct {
	Type   string `json:"type"`
	Room   string `json:"room"`
	Stream string `json:"stream"`
	Audio  *bool  `json:"audio,omitempty"`
	Video  *bool  `json:"video,omitempty"`
	Pub    *bool  `json:"pub,omitempty"`
	Share  *bool  `json:"share,omitempty"`
}

type updateMessage struct {
	Type  string `json:"type"`
	Audio *bool  `json:"audio,omitempty"`
	Video *bool  `json:"video,omitempty"`
	Pub   *bool  `json:"pub,omitempty"`
	Share *bool  `json:"share,omitempty"`
}

type leaveMessage struct {
	Type string `json:"type"`
}

type pingMessage struct {
	Type string `json:"type"`
}

type chatMessage struct {
	Type    string `json:"type"`
	Room    string `json:"room"`
	Content string `json:"content"`
	MsgID   string `json:"msg_id"`
}

type whiteboardMessage struct {
	Type     string `json:"type"`
	Room     string `json:"room"`
	Op       string `json:"op"`
	MsgID    string `json:"msg_id"`
	Stream   string `json:"stream"`
	StrokeID string `json:"stroke_id"`
	TS       int64  `json:"ts"`
}

type cmdMessage struct {
	Type   string `json:"type"`
	Room   string `json:"room"`
	To     string `json:"to"`
	Action string `json:"action"`
}

type authFailMessage struct {
	Type string `json:"type"`
	Code string `json:"code"`
	Msg  string `json:"msg"`
}

type authOKMessage struct {
	Type string `json:"type"`
	User string `json:"user"`
	Role string `json:"role"`
}

type authRequiredMessage struct {
	Type string `json:"type"`
	Msg  string `json:"msg"`
}

type authRegisteredMessage struct {
	Type string `json:"type"`
	User string `json:"user"`
}

type pongMessage struct {
	Type string `json:"type"`
}

type ctrlMessage struct {
	Type   string `json:"type"`
	Action string `json:"action"`
	To     string `json:"to"`
	By     string `json:"by"`
}

type memberSnapshot struct {
	User   string `json:"user"`
	Stream string `json:"stream"`
	Audio  bool   `json:"audio"`
	Video  bool   `json:"video"`
	Pub    bool   `json:"pub"`
	Share  bool   `json:"share"`
	Role   string `json:"role"`
}

type membersMessage struct {
	Type    string           `json:"type"`
	Room    string           `json:"room"`
	Members []memberSnapshot `json:"members"`
	WBLock  bool             `json:"wb_lock"`
}

type chatBroadcastMessage struct {
	Type    string `json:"type"`
	Room    string `json:"room"`
	User    string `json:"user"`
	Stream  string `json:"stream"`
	Content string `json:"content"`
	MsgID   string `json:"msg_id"`
	TS      int64  `json:"ts"`
	Ver     int    `json:"ver"`
}

func boolOr(v *bool, def bool) bool {
	if v == nil {
		return def
	}
	return *v
}

func nowMs() int64 {
	return time.Now().UnixMilli()
}
