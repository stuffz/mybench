// Single seam over the generated wails bindings: components import services
// and wire types from here, never from the generated paths directly.

export { Service as ConnectionService } from "../../bindings/github.com/stuffz/mybench/internal/conn";
export { Service as QueryService } from "../../bindings/github.com/stuffz/mybench/internal/query";
export { Service as AdminService } from "../../bindings/github.com/stuffz/mybench/internal/admin";
export { Service as WorkspaceService } from "../../bindings/github.com/stuffz/mybench/internal/workspace";
export { Service as MCPService } from "../../bindings/github.com/stuffz/mybench/internal/mcp";
export type { Status as MCPStatus } from "../../bindings/github.com/stuffz/mybench/internal/mcp";

export type {
  SavedConn,
  State as ConnState,
  TeleportStatus,
  TeleportDB,
  SSHAgentStatus,
  SSHBrowse,
  SSHFile,
} from "../../bindings/github.com/stuffz/mybench/internal/conn";
export type {
  CellEdit,
  Column,
  EditInfo,
  ResultState,
  RowWindow,
  RunResult,
} from "../../bindings/github.com/stuffz/mybench/internal/query";
export type { HistoryEntry, Snippet } from "../../bindings/github.com/stuffz/mybench/internal/storage";
export type {
  AppInfo,
  Process,
  UserRow,
  StatusSnapshot,
  SchemaTable,
  SchemaColumn,
  IndexRow,
  ForeignKey,
  TableInfo,
  SchemaMeta,
  KV,
  ServerInfo,
  Graph,
  GraphNode,
  GraphEdge,
} from "../../bindings/github.com/stuffz/mybench/internal/admin";
