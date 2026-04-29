# Skills (Agent Playbooks)

This folder contains subproject workflows that agents load on demand. Keep `AGENTS.md` small; put task-specific steps,
commands, caveats, and examples here.

## Contents

| Skill | Description |
| --- | --- |
| `build/` | Configure and build libatapp with CMake |
| `testing/` | Run and write private-framework unit tests |
| `configure-expression/` | Edit environment-expression-enabled config fields and syntax |
| `libatapp-module-connector/` | Work on modules, connectors, endpoints, routing, and lifecycle hooks |
| `libatapp-etcd-discovery/` | Work on etcd client, discovery sets, topology, keepalive, and watchers |
| `ai-agent-maintenance/` | Audit and optimize AI agent prompts, bridge files, and skills |

## When to read what

- If you want to **build**: start with `build/SKILL.md`.
- If you want to **run or write unit tests**: start with `testing/SKILL.md`.
- If you are editing config expression behavior: see `configure-expression/SKILL.md`.
- If you are working on modules, connectors, endpoints, or routing: see `libatapp-module-connector/SKILL.md`.
- If you are working on etcd discovery, topology, keepalive, or watchers: see `libatapp-etcd-discovery/SKILL.md`.
- If you are updating AI agent prompts or skills: see `ai-agent-maintenance/SKILL.md`.

## Maintenance rules

- Folder name and frontmatter `name` must match.
- `description` is the discovery surface: start with `Use when:` and include concrete trigger words.
- Keep each `SKILL.md` focused; move bulky examples or reference material into sibling files when needed.
