NATIVE_APPLY_MODE ?= mesh_first_paint
RESEARCH_ARTIFACTS ?= $(MECCHA_RESEARCH_ARTIFACTS)
VERSION ?= $(shell git describe --tags --exact-match 2>/dev/null || git describe --tags --dirty --always 2>/dev/null || printf dev)
BUILD_PS := scripts/build.ps1
RUN_PS := scripts/dev.ps1
PACKAGE_PS := scripts/release.ps1
MESH_PS := scripts/mesh.ps1
RESEARCH_ARTIFACT_FLAGS := $(if $(filter 1 true TRUE yes YES on ON,$(RESEARCH_ARTIFACTS)),-EnableResearchArtifacts,)
MESH_ARGS := $(if $(PAKS),-PaksPath "$(PAKS)",) $(if $(MAPPINGS),-MappingsPath "$(MAPPINGS)",) $(if $(CUE4PARSE),-Cue4ParsePath "$(CUE4PARSE)",) $(if $(GAME_VERSION),-GameVersion "$(GAME_VERSION)",) $(if $(OODLE),-OodlePath "$(OODLE)",) $(if $(ZLIB),-ZlibPath "$(ZLIB)",)

.PHONY: build run dev package mesh clean

define RUN_POWERSHELL
	@if command -v pwsh >/dev/null 2>&1; then \
		pwsh -NoProfile -ExecutionPolicy Bypass -File $(1) $(2); \
	elif command -v powershell.exe >/dev/null 2>&1; then \
		PS_SCRIPT_WIN="$$(if command -v wslpath >/dev/null 2>&1; then wslpath -w $(1); else printf '%s' $(1); fi)"; \
		powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$$PS_SCRIPT_WIN" $(2); \
	else \
		echo "PowerShell runtime not found." >&2; exit 127; \
	fi
endef

build:
	$(call RUN_POWERSHELL,$(BUILD_PS),-Version $(VERSION))

run: build
	$(call RUN_POWERSHELL,$(RUN_PS),-NativeApplyMode $(NATIVE_APPLY_MODE) $(RESEARCH_ARTIFACT_FLAGS))

dev: run

package: build
	$(call RUN_POWERSHELL,$(PACKAGE_PS),-Version $(VERSION))

mesh:
	$(call RUN_POWERSHELL,$(MESH_PS),$(MESH_ARGS))

clean:
	rm -rf .build
