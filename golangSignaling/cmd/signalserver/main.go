package main

import (
	"log"
	"net/http"

	"golangsignaling/internal/config"
	"golangsignaling/internal/server"
	"golangsignaling/internal/storage"
)

func main() {
	cfg, err := config.Load()
	if err != nil {
		log.Fatalf("load config failed: %v", err)
	}

	store, err := storage.OpenMySQL(cfg)
	if err != nil {
		log.Fatalf("open mysql failed: %v", err)
	}
	defer store.Close()

	hub := server.NewHub(store)

	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ok"))
	})
	mux.HandleFunc("/", hub.ServeWS)

	log.Printf("Go signal server listening on ws://%s", cfg.ListenAddr())

	if err := http.ListenAndServe(cfg.ListenAddr(), mux); err != nil {
		log.Fatalf("listen failed: %v", err)
	}
}
