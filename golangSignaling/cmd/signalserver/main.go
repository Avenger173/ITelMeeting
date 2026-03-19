package main

import (
	"context"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"golangsignaling/internal/config"
	"golangsignaling/internal/server"
	"golangsignaling/internal/storage"
)

func main() {
	cfg, err := config.Load()
	if err != nil {
		log.Fatalf("load config failed: %v", err)
	}
	log.Printf("[Config] loaded %s", cfg.StartupSummary())

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
	mux.HandleFunc("/ws", hub.ServeWS)

	srv := &http.Server{
		Addr:              cfg.ListenAddr(),
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
	}

	errCh := make(chan error, 1)
	go func() {
		log.Printf("Go signal server listening on ws://%s", cfg.ListenAddr())
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			errCh <- err
		}
	}()

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	select {
	case err := <-errCh:
		log.Fatalf("listen failed: %v", err)
	case sig := <-sigCh:
		log.Printf("shutdown signal received: %s", sig.String())
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := srv.Shutdown(ctx); err != nil {
			log.Printf("graceful shutdown failed: %v", err)
		}
	}
}
