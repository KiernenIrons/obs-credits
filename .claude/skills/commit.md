---
name: commit
description: Stage changes, generate a detailed commit message, and commit as Kiernen Irons
user_invocable: true
---

# Commit - OBS Credits Plugin

You are a git commit assistant for the OBS Credits plugin project. The sole author is **Kiernen Irons**. Every commit must be attributed to them.

## Steps

1. **Check repo status**: Run `git status` and `git diff --stat` to understand what changed. If this is not yet a git repo, run `git init` first.

2. **Review all changes in detail**: Run `git diff` for unstaged changes and `git diff --cached` for already-staged changes. Read any new untracked files listed by `git status`.

3. **Build a change log**: For every file that was added, modified, or deleted, write a one-line summary of what changed and why. Group changes by category:
   - **feat**: New functionality
   - **fix**: Bug fixes
   - **refactor**: Code restructuring without behavior change
   - **docs**: Documentation updates
   - **config**: Build system, CI, tooling, or settings changes
   - **style**: Formatting, naming, or comment-only changes

4. **Stage all relevant files**: Run `git add` for each changed file individually. Never use `git add -A` or `git add .`. Do NOT stage files that look like secrets (`.env`, credentials, API keys) - warn the user instead.

5. **Compose the commit message** using this format:

```
<type>(<scope>): <short summary under 72 chars>

<Detailed description of what changed and why, wrapped at 72 chars.
Reference specific files and functions where helpful.>

Changes:
- <file1>: <what changed>
- <file2>: <what changed>
- ...

Author: Kiernen Irons
```

6. **Set author and commit**:
```bash
git commit --author="Kiernen Irons <kiernen@users.noreply.github.com>" -m "<message>"
```

Always pass the commit message via HEREDOC for proper formatting:
```bash
git commit --author="Kiernen Irons <kiernen@users.noreply.github.com>" -m "$(cat <<'EOF'
<full commit message here>
EOF
)"
```

7. **Verify**: Run `git log -1 --format=fuller` to confirm the commit was created with the correct author.

## Rules

- **Kiernen Irons is the ONLY author** - never use any other name or the Co-Authored-By trailer
- Never amend a previous commit - always create a new one
- Never force push
- Never commit `.env`, credential files, or build artifacts
- If no changes exist, tell the user "Nothing to commit" and stop
- Keep the short summary line under 72 characters
- Use present tense ("add feature" not "added feature")
