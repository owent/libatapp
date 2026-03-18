# Skills (Agent Playbooks)

Actionable guides for common workflows in this repo.

Each skill is a directory containing a `SKILL.md` file with YAML frontmatter, following the [Agent Skills](https://agentskills.io/) specification.

| Skill | Directory | Description |
| ----- | --------- | ----------- |
| Build | `build/` | Configure/build with CMake |
| Testing | `testing/` | Run and write unit tests (incl. Windows DLL/PATH notes, multi-node patterns, debug time control) |
| Configure Expression | `configure-expression/` | Environment-variable expression expansion in config fields |
| Module & Connector | `libatapp-module-connector/` | Module lifecycle, connector architecture, endpoint management, connection handles, message routing patterns |
| etcd & Discovery | `libatapp-etcd-discovery/` | etcd client, service discovery sets, topology management, keepalive, watchers, etcd_module integration |
