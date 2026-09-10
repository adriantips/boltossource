# Saves any pending work in the boltOS repo, then shuts the machine down
# with a cancellable countdown. Triggered once by a self-removing Stop hook
# (see .claude/settings.local.json) after the current Claude Code task ends.

$ErrorActionPreference = "Stop"
$repoRoot = "C:\Users\adria\Desktop\Coding\boltOS"
Set-Location $repoRoot

$messages = @()

try {
    git add -A
    git diff --cached --quiet
    if ($LASTEXITCODE -ne 0) {
        $commitMsg = "auto-save: WIP snapshot before shutdown`n`nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
        git commit -m $commitMsg | Out-Null
        $messages += "Committed pending changes as a WIP snapshot."
    } else {
        $messages += "Nothing to save - working tree was already clean."
    }
} catch {
    $messages += "Warning: git save step failed ($($_.Exception.Message)) - shutdown will still proceed."
}

# 60s cancellable countdown. From another terminal: shutdown /a
shutdown /s /t 60 /c "Claude Code task finished - shutting down in 60s. Run 'shutdown /a' to cancel."
$messages += "Shutdown scheduled in 60s. Run 'shutdown /a' to cancel."

$summary = ($messages -join " ") -replace '"', '\"'
Write-Output "{`"systemMessage`": `"$summary`"}"
