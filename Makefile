# Manual build, for use inside an MSYS2 MINGW64 shell.
#
#   make GKRELLM_EXE="/c/Program Files/GKrellM/gkrellm.exe" \
#        SOURCE_TREE=./work/gkrellm-2.5.1
#
# For the fully automated path use build.bat instead.

GKRELLM_EXE ?= /c/Program Files/GKrellM/gkrellm.exe
SOURCE_TREE ?= ./work/gkrellm-2.5.1
OUT         ?= ./build
PLUGIN_DIR  ?= $(HOME)/.gkrellm2/plugins

.PHONY: all clean install

all:
	@bash scripts/compile.sh \
		--gkrellm-exe "$(GKRELLM_EXE)" \
		--source-tree "$(SOURCE_TREE)" \
		--project . \
		--out "$(OUT)"

install: all
	@mkdir -p "$(PLUGIN_DIR)"
	@cp -f "$(OUT)/gkrellm-nvidia.dll" "$(PLUGIN_DIR)/"
	@echo "installed to $(PLUGIN_DIR)"

clean:
	@rm -rf "$(OUT)"
	@echo "cleaned"
