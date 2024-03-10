module github.com/walterschell/clamav-dbscan/watcher

go 1.21.3

require (
	github.com/fatih/color v1.16.0
	github.com/walterschell/clamav-dbscan/watcher/fanotify v0.0.99
)

require (
	github.com/mattn/go-colorable v0.1.13 // indirect
	github.com/mattn/go-isatty v0.0.20 // indirect
	github.com/yookoala/realpath v1.0.0 // indirect
	golang.org/x/sys v0.18.0 // indirect
)

replace github.com/walterschell/clamav-dbscan/watcher/fanotify => ./fanotify
