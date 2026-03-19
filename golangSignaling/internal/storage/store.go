package storage

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"time"

	"golangsignaling/internal/config"

	_ "github.com/go-sql-driver/mysql"
)

type Store struct {
	db *sql.DB
}

type User struct {
	User         string
	PasswordHash string
	Role         string
}

func OpenMySQL(cfg config.Config) (*Store, error) {
	db, err := sql.Open("mysql", cfg.DSN())
	if err != nil {
		return nil, err
	}

	db.SetMaxOpenConns(20)
	db.SetMaxIdleConns(5)
	db.SetConnMaxLifetime(30 * time.Minute)

	if err := db.Ping(); err != nil {
		_ = db.Close()
		return nil, err
	}

	s := &Store{db: db}
	if err := s.InitSchema(); err != nil {
		_ = db.Close()
		return nil, err
	}
	return s, nil
}

func (s *Store) Close() error {
	return s.db.Close()
}

func (s *Store) InitSchema() error {
	const usersSQL = `
	CREATE TABLE IF NOT EXISTS users(
		id BIGINT PRIMARY KEY AUTO_INCREMENT,
		user VARCHAR(128) NOT NULL,
		password_hash VARCHAR(128) NOT NULL,
		role VARCHAR(32) NOT NULL DEFAULT 'member',
		created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
	) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
	`
	const eventsSQL = `
	CREATE TABLE IF NOT EXISTS meeting_events(
		id BIGINT PRIMARY KEY AUTO_INCREMENT,
		ts_ms BIGINT NOT NULL,
		room VARCHAR(128) NOT NULL,
		actor_user VARCHAR(128) NULL,
		actor_stream VARCHAR(191) NULL,
		target_stream VARCHAR(191) NULL,
		event_type VARCHAR(64) NOT NULL,
		payload_json LONGTEXT NULL,
		INDEX idx_meeting_events_room_ts(room,ts_ms)
	)ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
		`

	if _, err := s.db.Exec(usersSQL); err != nil {
		return fmt.Errorf("create users failed: %w", err)
	}
	if _, err := s.db.Exec(eventsSQL); err != nil {
		return fmt.Errorf("create meeting_events failed: %w", err)
	}

	return nil
}

func (s *Store) RegisterUser(user, passwordHash, role string) error {
	_, err := s.db.Exec(
		`INSERT INTO users(user, password_hash, role, created_at) VALUES(?,?,?,CURRENT_TIMESTAMP)`,
		user,
		passwordHash,
		role,
	)
	return err
}

func (s *Store) FindUser(user string) (User, bool, error) {
	var out User
	row := s.db.QueryRow(`SELECT user, password_hash, role FROM users WHERE user = ?`, user)
	err := row.Scan(&out.User, &out.PasswordHash, &out.Role)
	if err == sql.ErrNoRows {
		return User{}, false, nil
	}
	if err != nil {
		return User{}, false, err
	}
	return out, true, nil
}

func (s *Store) AppendMeetingEvent(
	room string,
	eventType string,
	actorUser string,
	actorStream string,
	targetStream string,
	payload map[string]any,
) error {
	raw := "{}"
	if payload != nil {
		b, err := json.Marshal(payload)
		if err != nil {
			return err
		}
		raw = string(b)
	}

	_, err := s.db.Exec(
		`INSERT INTO meeting_events(
			ts_ms, room, actor_user, actor_stream, target_stream, event_type, payload_json
		) VALUES(?,?,?,?,?,?,?)`,
		time.Now().UnixMilli(),
		room,
		nullableString(actorUser),
		nullableString(actorStream),
		nullableString(targetStream),
		eventType,
		raw,
	)
	return err
}

func nullableString(s string) any {
	if s == "" {
		return nil
	}
	return s
}
