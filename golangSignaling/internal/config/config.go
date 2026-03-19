package config

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	ini "gopkg.in/ini.v1"
)

type Config struct {
	ListenHost string
	ListenPort int

	DBDriver   string
	DBName     string
	DBHost     string
	DBPort     int
	DBUser     string
	DBPassword string
}

func ResolvePath() string {
	if p := strings.TrimSpace(os.Getenv("SMARTMEET_GO_SIGNAL_CONFIG")); p != "" {
		return p
	}

	exePath, err := os.Executable()
	if err == nil {
		exeDir := filepath.Dir(exePath)
		candidate := filepath.Join(exeDir, "signalserver.ini")
		if _, statErr := os.Stat(candidate); statErr == nil {
			return candidate
		}
	}

	return "signalserver.ini"
}

func Load() (Config, error) {
	path := ResolvePath()

	cfgFile, err := ini.Load(path)
	if err != nil {
		return Config{}, err
	}

	cfg := Config{
		ListenHost: cfgFile.Section("server").Key("listen_host").MustString("0.0.0.0"),
		ListenPort: cfgFile.Section("server").Key("listen_port").MustInt(9002),

		DBDriver:   strings.ToUpper(cfgFile.Section("database").Key("driver").MustString("QMYSQL")),
		DBName:     cfgFile.Section("database").Key("database_name").MustString("smartmeet"),
		DBHost:     cfgFile.Section("database").Key("host").MustString("127.0.0.1"),
		DBPort:     cfgFile.Section("database").Key("port").MustInt(3306),
		DBUser:     cfgFile.Section("database").Key("user").MustString("smartmeet"),
		DBPassword: cfgFile.Section("database").Key("password").MustString(""),
	}

	if cfg.DBDriver != "QMYSQL" {
		return Config{}, fmt.Errorf("first GO version only supports QMYSQL,got %s", cfg.DBDriver)
	}

	return cfg, nil
}

func (c Config) ListenAddr() string {
	return fmt.Sprintf("%s:%d", c.ListenHost, c.ListenPort)
}

func (c Config) DSN() string {
	return fmt.Sprintf(
		"%s:%s@tcp(%s:%d)/%s?charset=utf8mb4&parseTime=True&loc=Local",
		c.DBUser,
		c.DBPassword,
		c.DBHost,
		c.DBPort,
		c.DBName,
	)
}
