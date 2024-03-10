module github.com/walterschell/clamav-dbscan/watcher

go 1.21.3

require (
	github.com/spf13/pflag v1.0.5 // indirect
	golang.org/x/sys v0.18.0 // indirect
	github.com/walterschell/clamav-dbscan/watcher/fanotify v0.0.99
)

replace github.com/walterschell/clamav-dbscan/watcher/fanotify => ./fanotify