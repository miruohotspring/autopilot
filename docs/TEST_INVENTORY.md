# Test Inventory

`autopilot` の現行自動テスト一覧です。すべて `tests/` 配下の Bash ベース統合テストで、ビルド済みの `./ap` を一時ディレクトリ上で実行して検証します。

## 実行方法

- `make test`
- `bash tests/run_all_tests.sh`

## 構成サマリ

- テストスクリプト数: 10
- 個別テストケース数: 65
- 実行順: `init` -> `update` -> `new` -> `delete` -> `list` -> `add` -> `rm` -> `start` -> `briefing` -> `kill`
- 主要なテスト手法:
  - 一時 `HOME` を使った隔離実行
  - `projects.yaml` や `TODO.md` の内容検証
  - fake `tmux` / fake agent (`claude`, `codex`) を使った外部依存のシミュレーション
  - runtime artifact (`runtime/state`, `runtime/events`, `runtime/runs`) の検証

## `tests/run_all_tests.sh`

- 各テストスクリプトを固定順で直列実行するランナーです。
- 個別テストの並列実行やフィルタ機能はありません。

## `tests/test_ap_init.sh`

対象: `ap init`

1. `~/.autopilot` が存在しない状態で、初期ディレクトリ群と managed skill を作成する。
2. 既存 `~/.autopilot` を `~/.autopilot.bak` へ退避し、新しい `~/.autopilot` を再作成する。
3. `~/.autopilot.bak` が既にある場合は失敗し、既存データを保持する。
4. `~/.codex` が存在する場合だけ managed skill の symlink を作成し、`~/.claude` がなければ触らない。
5. 引数なし実行や未知コマンドなどの不正呼び出しを非ゼロ終了にする。

## `tests/test_ap_update.sh`

対象: `ap update`

1. `~/.autopilot` がない場合は `ap init` を促して失敗する。
2. リポジトリルートから実行したとき、`templates/autopilot` の managed file を `~/.autopilot` へ同期する。
3. `~/.codex/skills` と `~/.claude/skills` の既存 managed skill ディレクトリを symlink に置き換え、他ディレクトリは保持する。
4. `~/.autopilot/skills` 配下の自己参照 symlink を実体ディレクトリへ修復する。
5. リポジトリルート以外から実行した場合は失敗する。

## `tests/test_ap_new.sh`

対象: `ap new`

1. `~/.autopilot` がない場合は `ap init` を促して失敗する。
2. `projects.yaml` がない状態でも生成し、project directory と `TODO.md` / `dashboard.md` を作成する。
3. 既存 project 名の重複作成を拒否する。
4. 先頭末尾ハイフンやアンダースコアを含む不正 project 名を拒否する。
5. project 名の対話入力モードで project を作成する。

## `tests/test_ap_delete.sh`

対象: `ap delete`

1. `~/.autopilot` がない場合は `ap init` を促して失敗する。
2. `projects.yaml` がない場合は `project not found` で失敗する。
3. 確認に `y` を返したとき project を削除する。
4. 確認に `n` を返したとき削除を中止し、非ゼロ終了にする。
5. project 名省略時の対話選択で削除対象を選べる。
6. 不正な project 名と存在しない project 名を拒否する。

## `tests/test_ap_list.sh`

対象: `ap list`

1. `~/.autopilot` がない場合は `ap init` を促して失敗する。
2. `projects.yaml` がない場合は `no projects` を出力する。
3. 既存 project 名をソート順で 1 行ずつ出力する。
4. 余分な引数を拒否する。

## `tests/test_ap_add.sh`

対象: `ap add`

1. `~/.autopilot` がない場合は `ap init` を促して失敗する。
2. 相対パスを絶対パスへ正規化し、`name/path` を `projects.yaml` に保存して symlink を作成する。
3. 同一 `name/path` の再追加を no-op として扱う。
4. `-p` 省略時に project を対話選択でき、不要な確認プロンプトを出さない。
5. 不正な project 名と存在しない project 名を拒否する。
6. `-n` 省略時の空入力を `main` として扱う。
7. 既に `main` がある場合は空入力を拒否し、明示名を必須にする。
8. `/` を含むような不正 path 名を拒否する。

## `tests/test_ap_rm.sh`

対象: `ap rm`

1. `~/.autopilot` がない場合は `ap init` を促して失敗する。
2. `projects.yaml` がない場合は `project not found` で失敗する。
3. `-p` 指定時に path を選んで確認後に削除できる。
4. 確認に `n` を返したとき削除を中止し、path を保持する。
5. `-p` 省略時に project と path の両方を対話選択できる。
6. path を持たない project を対象にした場合は失敗する。
7. 不正な project 名と存在しない project 名を拒否する。

## `tests/test_ap_start.sh`

対象: `ap start`

1. `~/.autopilot` がない場合は `ap init` を促して失敗する。
2. project 名省略時の対話選択で対象 project を選び、完了した TODO を `[x]` に更新する。
3. 正常実行時に `runtime/runs`, `runtime/state`, `runtime/events` を作成し、最初の未完了タスクを `done` にする。
4. `TODO.md` から消えたタスクを `present_in_todo: false` として保持し、再実行対象から外す。
5. agent が失敗した場合は `TODO.md` を変更せず、失敗結果とログを runtime artifact に残す。
6. 失敗済みタスクを次回実行で再選択し、`attempt_count` を増やして完了へ遷移させる。
7. project に複数 path があり `main` がない場合は開始を拒否する。
8. 実行可能な TODO 項目がない場合は失敗し、state 側には done 状態を同期する。
9. `config.toml` の `start.agent` に従って `claude` / `codex` の実行 CLI を切り替える。
10. 設定された agent CLI が `PATH` にない場合は、他 agent があっても失敗する。
11. 一度完了した TODO を `[ ]` に戻した場合、state を `todo` に戻して再実行できる。
12. agent 実行中に `TODO.md` が別変更されて競合した場合、task state は `done` のまま保ち、`todo_update_applied: false` と競合イベントを記録する。
13. 未完了 TODO に同名タイトルが複数ある場合は Phase 2 制約として失敗させる。
14. 起動前のイベント書き込み失敗時は `in_progress` への遷移をロールバックし、既存の stale `in_progress` タスクは再実行で回復する。
15. 実行後の event replay 失敗時も task を `in_progress` に残さず、次回 stale run 扱いにしない。
16. 出力中に blocker marker 文字列を引用しても、行頭 marker でなければ `blocked` にしない。
17. 同一タイトルの履歴タスクが複数残っている場合、既存 ID を再利用せず新しい task ID を採番する。

## `tests/test_ap_briefing.sh`

対象: `ap briefing`

1. tmux 外から実行した場合、新しい `autopilot` session と `colonel` window を作り attach する。
2. tmux 内から実行した場合、session 作成後に `switch-client` を使って移動する。
3. 既存 session に `colonel` window がある場合は再利用し、再作成しない。
4. 既存 session に `colonel` window がない場合は window を追加して attach する。

補足:

- fake `tmux` 経由で `claude --model sonnet --dangerously-skip-permissions` の送信内容も検証している。
- `日本語`, `$ap-self-recognition`, `$ap-briefing` を含む起動コマンドが組み立てられることも確認している。

## `tests/test_ap_kill.sh`

対象: `ap kill`

1. `autopilot` session が存在しない場合でも成功し、`kill-session` を呼ばない。
2. `autopilot` session が存在する場合は `kill-session` を呼んで終了する。
3. `kill-session` が失敗した場合は非ゼロ終了し、エラーメッセージを返す。
4. 余分な引数を拒否する。
