-- Loads the native plugin once; repeat calls are no-ops in SuperBLT.
if not _G.CantedFixLoaded then
	_G.CantedFixLoaded = true
	local ok, err = pcall(blt.load_native, ModPath .. "canted_fix.dll")
	if not ok then
		log("[CantedFix] failed to load canted_fix.dll: " .. tostring(err))
	end
end
