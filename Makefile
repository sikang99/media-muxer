#
# Makefile
#
all: usage

edit-readme er:
	vim README.md

edit-make em:
	vim Makefile

install-pkg ip:
	sudo apt install libsdl-dev libavformat-dev libavcodec-dev libavutil-dev

build b:
	go build

run r:
	go run


git  g:
	@echo ""
	@echo "make (git) [up|set]"
	@echo ""

git-up gu:
	git add README.md Makefile *.go media/ media2/
	git commit -m "Modified to use the latest libav library"
	git push origin master

git-set gs:
	git remote set-url origin https://sikang99:----@github.com/sikang99/media-muxer.git
