# Phase 1 Spec: `ap start` の最小実装

## 1. 位置づけ

`docs/PLAN.md` の Phase 1 を、実装に着手できる粒度まで具体化した仕様書である。

Phase 1 の役割は以下に限定する。

- `ap start` を CLI に追加する
- project から 1 件の task を選ぶ
- 1 回だけ agent を起動する
- 実行結果を後で見返せる形で保存する
- 成功時のみ `TODO.md` を更新する

この段階では、event/state/reviewer/retry/approval/loop 実行は導入しない。

## 2. ゴールと非ゴール

### 2.1 ゴール

- `ap start [project_name]` が動作する
- 対象 project の `TODO.md` から未完了 task を 1 件選べる
- 選ばれた task を対象に agent を 1 回実行できる
- 実行ログとメタ情報を `runtime/` 配下に保存できる
- 実行成功時は `TODO.md` の該当 task を完了に更新できる

### 2.2 非ゴール

- 複数 task の連続実行
- reviewer 起動
- task 自動生成
- task/state/event の本格導入
- alert 作成
- lock/retry/timeout の高度化
- 人間承認フロー

## 3. CLI 仕様

### 3.1 コマンド

```text
ap start [project_name]
```

### 3.2 引数解決

- `project_name` 指定あり:
  - project 名の形式を既存コマンドと同じ規則で検証する
  - 該当 project が存在しなければ失敗する
- `project_name` 指定なし:
  - 既存の `ap add` / `ap delete` と同様に project 選択 UI を使う
  - TTY では対話選択、非 TTY では番号選択にフォールバックする

### 3.3 終了コード

- `0`: task 実行成功
- `1`: それ以外すべて

Phase 1 では細かい終了コードは分けない。

## 4. 前提条件

`ap start` は以下を満たさない場合に失敗する。

- `$HOME/.autopilot` が存在する
- 対象 project が存在する
- project に作業対象 path が存在する
- `TODO.md` に未完了 task が存在する
- 起動可能な agent CLI が見つかる

エラー時は既存コマンドに合わせて簡潔なメッセージを stderr に出す。

## 5. 作業対象 path の決定

Phase 1 では複数 path を賢く扱わない。working directory は以下の優先順位で 1 つだけ選ぶ。

1. name が `main` の managed path があればそれを使う
2. `main` がなく、managed path が 1 件だけならそれを使う
3. それ以外は失敗する

失敗時メッセージ例:

```text
ap start failed: project has multiple paths and no 'main'
```

理由:

- Phase 1 では ambiguity を避けたい
- path 選択 UI や path ごとの task routing は Phase 2 以降に回す

## 6. `TODO.md` の最小仕様

Phase 1 では `TODO.md` を task source of truth とみなす。

### 6.1 対象にする行

task として認識するのは、以下の形式に一致する 1 行のみ。

```text
- [ ] <task title>
```

完了済みは以下とする。

```text
- [x] <task title>
```

### 6.2 Phase 1 でサポートしないもの

- 番号付きリスト
- ネストした checklist の意味解釈
- priority 記法
- dependency 記法
- metadata コメント
- Markdown heading からの意味抽出

### 6.3 task 選択ルール

- `TODO.md` を上から順に走査する
- 最初に見つかった `- [ ] ...` を選ぶ
- 選択結果として以下を保持する
  - `task_title`
  - `source_file` (`TODO.md`)
  - `source_line`
  - `original_line_text`

これは最小実装として十分であり、priority/dependency 導入前でも deterministic に動く。

## 7. 実行フロー

`ap start` の処理順は以下。

1. `~/.autopilot` と対象 project の存在確認
2. project 選択
3. managed path 解決
4. `TODO.md` から task 選択
5. `runtime/` と run directory 作成
6. agent へ渡す prompt 生成
7. agent 実行
8. stdout/stderr と実行メタ情報を保存
9. 成功時のみ `TODO.md` の該当 task を `[x]` に更新
10. `result.json` と `last_run.json` を保存

Phase 1 では 1 回実行したら必ず終了する。

## 8. runtime 保存仕様

Phase 1 では Phase 2 の最終形を先取りしすぎず、将来移行しやすい最小構成だけ作る。

### 8.1 ディレクトリ構成

```text
$HOME/.autopilot/projects/<project_name>/
  TODO.md
  dashboard.md
  runtime/
    last_run.json
    runs/
      run-YYYYMMDD-HHMMSS-LNN/
        meta.json
        prompt.txt
        stdout.log
        stderr.log
        result.json
```

`LNN` は `TODO.md` の行番号を表す。

### 8.2 `meta.json`

実行開始前に作成し、終了時に追記更新する。

想定フィールド:

```json
{
  "run_id": "run-20260314-120000-L7",
  "project": "demo",
  "task_title": "Add ap start command",
  "task_source_file": "TODO.md",
  "task_source_line": 7,
  "task_original_line": "- [ ] Add ap start command",
  "path_name": "main",
  "working_directory": "/abs/path/to/repo",
  "agent": "claude",
  "status": "running",
  "started_at": "2026-03-14T12:00:00+09:00",
  "ended_at": null,
  "exit_code": null
}
```

終了後:

- `status` を `succeeded` または `failed` に更新する
- `ended_at` を埋める
- `exit_code` を埋める

### 8.3 `result.json`

実行終了後に書く最終結果ファイル。

想定フィールド:

```json
{
  "run_id": "run-20260314-120000-L7",
  "status": "succeeded",
  "exit_code": 0,
  "started_at": "2026-03-14T12:00:00+09:00",
  "ended_at": "2026-03-14T12:04:12+09:00",
  "duration_ms": 252000,
  "todo_update_applied": true,
  "summary_excerpt": "Implemented ap start skeleton and added tests."
}
```

`summary_excerpt` は strict な構造化出力を要求しない。Phase 1 では以下のどちらかでよい。

- agent の最終 stdout から末尾数行を抜粋する
- 取れなければ空文字にする

### 8.4 `last_run.json`

project 単位の最新 run 参照用。中身は `result.json` と同等またはそのサブセットでよい。

目的は「直近何が起きたか」をすぐ見られるようにすること。

## 9. agent wrapper 仕様

Phase 1 の public CLI は `ap start` のみで、agent 選択オプションはまだ出さない。
agent の選択は `~/.autopilot/config.toml` の `[start].agent` で指定可能とし、未指定時だけ自動検出へフォールバックする。

### 9.1 内部インターフェース

内部的には以下の責務を持つ wrapper を追加する。

- 利用可能な agent CLI を検出する
- 共通 prompt を組み立てる
- 指定 working directory で agent を 1 回起動する
- stdout/stderr をファイルへ保存する
- 終了コードを返す

### 9.2 agent 選択方針

Phase 1 の設計判断は以下。

- 実装上は wrapper を用意する
- ただし最初の実装は 1 agent だけでもよい
- `claude` と `codex` の両対応は wrapper の差し替えで拡張可能にする
- 明示設定がある場合はその agent を優先し、見つからなければ失敗する

懸念点:

- `claude` の CLI 呼び出し方は現 repo に実例がある
- `codex` の非対話実行方法はこの時点で repo 内に検証済み実例がない

そのため、Phase 1 の完了条件は「wrapper 経由で少なくとも 1 つの agent を確実に起動できること」とし、`codex` は同 interface に後続で追加可能な形にしておくのが安全である。

### 9.3 prompt の最低限の内容

`prompt.txt` には少なくとも以下を入れる。

- project 名
- working directory
- 選択 task の title
- `TODO.md` から抜いた該当行
- 実装対象 path 情報
- 「必要ならファイルを編集し、最後に短い要約を出す」こと

agent ごとの高度な system prompt 最適化は Phase 1 のスコープ外。

## 10. `TODO.md` 更新ルール

### 10.1 成功時

agent exit code が `0` のときだけ、選択された task 行を:

```text
- [ ] ...
```

から:

```text
- [x] ...
```

に更新する。

### 10.2 失敗時

- `TODO.md` は更新しない
- task は未完了のまま残す

### 10.3 更新競合

run 中に `TODO.md` が別変更される可能性があるため、更新時は以下を確認する。

- 選択時の `source_line` がまだ存在する
- その行の内容が `original_line_text` と一致する

一致しない場合:

- `TODO.md` は変更しない
- `result.json` の `todo_update_applied` を `false` にする

これは Phase 1 でできる最小限の衝突回避である。

## 11. stdout/stderr の扱い

- child process の stdout/stderr はそれぞれ `stdout.log` / `stderr.log` に保存する
- 可能なら親プロセスの stdout/stderr にもそのまま流す
- 保存は常に行い、成功失敗に関係なく run directory を残す

目的は「失敗時でも何が起きたか見返せる」こと。

## 12. エラー仕様

想定する主な失敗ケース:

- `~/.autopilot` が未初期化
- project が存在しない
- managed path が 0 件
- managed path が複数あり `main` がない
- `TODO.md` が存在しない
- 未完了 task がない
- agent CLI が見つからない
- run directory 作成に失敗
- child process が非 0 終了

stderr メッセージ例:

```text
Please run ap init first
project not found
ap start failed: no managed path
ap start failed: project has multiple paths and no 'main'
ap start failed: no runnable task found in TODO.md
ap start failed: no supported agent CLI found
```

メッセージ文面は既存コマンドに寄せて短く保つ。

## 13. 並行実行の扱い

Phase 1 では lock を導入しない。

そのため以下を仕様として明示する。

- 同一 project に対する同時 `ap start` は未サポート
- 挙動は未定義とみなす
- lock/retry は Phase 5 で導入する

## 14. 推奨実装単位

過剰抽象化は避けつつ、責務分離はしておく。

最低限の追加候補:

- `src/commands/cmd_start.cpp`
- `include/autopilot/commands/cmd_start.hpp`
- `src/projects/todo_task_selector.cpp`
- `include/autopilot/projects/todo_task_selector.hpp`
- `src/agents/agent_launcher.cpp`
- `include/autopilot/agents/agent_launcher.hpp`

既存修正箇所:

- `src/main.cpp`
- `CMakeLists.txt`
- `tests/run_all_tests.sh`
- `tests/test_ap_start.sh`

### 14.1 `cmd_start`

責務:

- CLI 引数解決
- project/path/task 解決のオーケストレーション
- runtime 作成
- agent launcher 呼び出し
- 成功時 TODO 更新
- 結果保存

### 14.2 `todo_task_selector`

責務:

- `TODO.md` から最初の未完了 task を選ぶ
- line number と original text を返す

### 14.3 `agent_launcher`

責務:

- agent 検出
- prompt 構築
- child process 起動
- stdout/stderr 保存
- exit code 返却

## 15. テスト観点

`tests/test_ap_start.sh` を追加し、少なくとも以下をカバーする。

- `~/.autopilot` がないと失敗する
- project 未指定時に project 選択できる
- project が存在しないと失敗する
- managed path がないと失敗する
- `main` 優先で working directory を選ぶ
- `main` がなく path が 1 件ならそれを使う
- 複数 path かつ `main` なしで失敗する
- `TODO.md` の最初の未完了 task を選ぶ
- 成功時に run directory, logs, result が作られる
- 成功時に選択 task が `[x]` になる
- 失敗時に `TODO.md` が変わらない
- task がないと失敗する
- 実行済み artifact が失敗時も残る

テストでは実 agent を呼ばず、`PATH` 先頭に fake agent script を置いて exit code と stdout/stderr を制御する。

## 16. Phase 2 への引き継ぎ

Phase 1 の保存形式は、Phase 2 以降で以下へ接続できるようにしておく。

- `runtime/runs/` はそのまま流用する
- `last_run.json` は後に `state/project.json` へ統合または置換可能
- `task_source_line` と `task_title` は後の task state 初期化に使える

重要なのは、Phase 1 で「実行した事実」と「対象 task」と「結果ログ」が必ず残ること。

## 17. 未解決事項

以下は実装前に軽く確認したい論点だが、Phase 1 着手の blocker ではない。

- `codex` CLI の非対話実行コマンドをどう確定するか
- parent stdout/stderr への live relay を最初から入れるか、まずは log 保存だけにするか
- `summary_excerpt` を単純抜粋にするか、agent へ固定フォーマット出力を求めるか

現時点の推奨は以下。

- agent wrapper は先に入れる
- 実動確認済みの agent から 1 つ繋ぐ
- result はまず exit code と raw log を正とする
