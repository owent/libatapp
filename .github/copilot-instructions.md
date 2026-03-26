# libatapp - VS Code Copilot Notes

<!-- The main project instructions are in the nearest AGENTS.md for this subproject. -->
<!-- This file contains only VS Code Copilot-specific notes (skills, path-specific references). -->
<!-- VS Code Copilot reads both this file and the nearest AGENTS.md automatically. -->

## Skills (How-to playbooks)

Operational, copy/paste-friendly guides live in `.agents/skills/`:

- Entry point: `.agents/skills/README.md`

| Skill              | Path                                                | Description                                    |
| ------------------ | --------------------------------------------------- | ---------------------------------------------- |
| Build              | `.agents/skills/build/SKILL.md`                     | Configure and build (Windows / Unix)           |
| Testing            | `.agents/skills/testing/SKILL.md`                   | Run unit tests (incl. Windows DLL/PATH notes)  |
| Config Expressions | `.agents/skills/configure-expression/SKILL.md`      | Environment-variable expression expansion      |
| Module & Connector | `.agents/skills/libatapp-module-connector/SKILL.md` | Module lifecycle, connector, endpoint, routing |
| etcd & Discovery   | `.agents/skills/libatapp-etcd-discovery/SKILL.md`   | etcd client, discovery, topology, keepalive    |

