# Phase 2 Spec: runtime/state/event の基礎導入

## 1. 位置づけ

`docs/PLAN.md` の Phase 2 を、実装に着手できる粒度まで具体化した仕様書である。

ただし `PLAN.md` では以下が別節で記述されている。

- Phase 2: runtime 構造の導入
- Phase 3: event モデルの導入
- Phase 4: task state の Markdown からの分離

実装上はこの 3 つを完全に分断するとかえって二度手間になるため、Phase 2 仕様では以下を 1 つの実装単位として扱う。

- `runtime/state/` の導入
- `runtime/events/events.jsonl` の導入
- `runtime/state/tasks/*.json` の導入
- `ap start` が state を読んで task を選び、結果を state/event に反映する流れ

一方で、retry/lock/reviewer/task 自動生成/alert はこの Phase では扱わない。

## 2. ゴールと非ゴール

### 2.1 ゴール

- `ap start [project_name]` が Phase 1 と同様に動作する
- project ごとに `runtime/state/project.json` を持てる
- task ごとに `runtime/state/tasks/<task_id>.json` を持てる
- 主要な操作を `runtime/events/events.jsonl` に append-only で残せる
- 次回起動時に既存 state を読み、task status / attempt 数 / latest run を引き継げる
- `TODO.md` を人間向けの入口として残しつつ、機械向けの truth を state へ移せる
- Phase 1 で作られた `runtime/runs/*` と `runtime/last_run.json` を継続利用できる

### 2.2 非ゴール

- lock 導入
- retry policy 導入
- timeout 管理
- reviewer 導入
- task dependency 解決
- task 自動生成
- alert 作成
- multi-path task routing の高度化
- `dashboard.md` の自動更新
- stdout/stderr の逐次 event 化

## 3. 基本方針

Phase 2 では source of truth を以下のように整理する。

- 人間向け入力: `TODO.md`
- 機械向け現在値: `runtime/state/project.json`, `runtime/state/tasks/*.json`
- 機械向け履歴: `runtime/events/events.jsonl`
- 生ログと実行成果物: `runtime/runs/*`

重要なのは、`TODO.md` を完全に捨てることではない。Phase 2 では以下を目指す。

- task の生成入口は引き続き `TODO.md`
- task の進行状態と試行回数は state で保持
- 実行履歴は event で保持
- `TODO.md` は state から独立に人間が編集できる

## 4. runtime ディレクトリ構成

Phase 2 の推奨構成は以下。

```text
$HOME/.autopilot/projects/<project_name>/
  TODO.md
  dashboard.md
  runtime/
    last_run.json
    events/
      events.jsonl
    state/
      project.json
      tasks/
        task-0001.json
        task-0002.json
    runs/
      run-YYYYMMDD-HHMMSS-LNN/
        meta.json
        prompt.txt
        stdout.log
        stderr.log
        result.json
```

Phase 1 の `runtime/runs/*` と `runtime/last_run.json` は維持する。

理由:

- 既存テストと互換性を保ちやすい
- run artifact はすでに使い道が明確
- state/event の追加だけで Phase 2 を進められる

## 5. `TODO.md` の扱い

### 5.1 Phase 2 で認識する形式

task として認識するのは Phase 1 と同じく以下のみ。

```text
- [ ] <task title>
- [x] <task title>
```

この Phase では以下を引き続き解釈しない。

- 番号付きリスト
- nested checklist の意味
- priority 記法
- dependency 記法
- hidden metadata comment
- heading からの意味抽出

### 5.2 `TODO.md` の役割

- 新規 task の入口
- 人間が現在の進行感を確認するためのビュー
- 手動で task を reopen / complete するための簡易操作面

ただし機械制御上の重要情報は state 側に置く。

## 6. task 同期モデル

Phase 2 では `ap start` 実行時に、最初に `TODO.md` と task state の同期を行う。

### 6.1 同期の目的

- `TODO.md` の checklist から task state を初期化・更新する
- Phase 1 時代の「Markdown だけが真実」という状態から移行する
- 後続の task 選択を state ベースで行えるようにする

### 6.2 同期アルゴリズム

`TODO.md` を上から順に走査し、各 checklist 行について以下を行う。

1. 既存 task state のうち、`source_file == "TODO.md"` かつ `source_line` と `title` が一致する task を探す
2. 見つかれば、その task を現在行に再関連付けする
3. 見つからない場合、未対応 task のうち `title` が一致する task が 1 件だけなら、それを再利用して `source_line` と `source_text` を更新する
4. それでも見つからなければ、新しい task id を払い出して新規 task として作成する

### 6.3 重複 title の制約

Phase 2 では、未完了 task に同一 title が複数あるケースを正確に同期しない。

仕様として以下を置く。

- 同一 title の未完了 task が複数ある場合、同期は失敗させてよい
- stderr には短いエラーメッセージを出す
- 例: `ap start failed: duplicate TODO task titles are not supported in Phase 2`

理由:

- 現行実装に task id を埋め込む仕組みがない
- JSON/YAML/Markdown 変換基盤を過剰に複雑化したくない

この制約は Phase 2 の意図的な簡略化である。

### 6.4 `TODO.md` から消えた task

既存 state に存在する task が今回の `TODO.md` に現れない場合、その task は削除しない。

扱いは以下。

- task state は残す
- `present_in_todo` を `false` にする
- `ap start` の選択対象から外す

この Phase では自動的に `cancelled` へ遷移させない。

### 6.5 checkbox と status の同期

同期時の基本ルールは以下。

- `- [ ]` の行は task を open とみなす
- `- [x]` の行は task を done とみなす

state への反映は以下。

- checkbox が `[ ]` で task status が `done` の場合は `todo` へ戻す
- checkbox が `[x]` で task status が `done` 以外の場合は `done` にする
- `attempt_count` や `latest_run_id` は checkbox 操作だけでは巻き戻さない

つまり、`TODO.md` は「task を reopen / complete する人間操作」として扱うが、run 履歴までは消さない。

## 7. task id と file 命名

新規 task id は以下の形式とする。

```text
task-0001
task-0002
```

ルール:

- 4 桁ゼロ埋めの連番
- 次番号は `runtime/state/tasks/` 内の既存 id を走査して決める
- 欠番は再利用しない

file 名は `<task_id>.json` とする。

## 8. `task state` 仕様

### 8.1 最低限のフィールド

各 `runtime/state/tasks/<task_id>.json` は最低限以下を持つ。

```json
{
  "id": "task-0001",
  "title": "first task",
  "status": "todo",
  "source_file": "TODO.md",
  "source_line": 3,
  "source_text": "- [ ] first task",
  "present_in_todo": true,
  "attempt_count": 0,
  "latest_run_id": null,
  "last_error": null,
  "created_at": "2026-03-14T12:00:00+09:00",
  "updated_at": "2026-03-14T12:00:00+09:00"
}
```

### 8.2 status 値

Phase 2 で扱う status は以下。

- `todo`
- `in_progress`
- `done`
- `failed`

以下は将来予約とするが、Phase 2 では自動生成しない。

- `blocked`
- `review_pending`
- `cancelled`

### 8.3 フィールドの意味

- `id`: task の内部識別子
- `title`: `TODO.md` の表示タイトル
- `status`: 現在の内部状態
- `source_file`: いったん `"TODO.md"` 固定
- `source_line`: 現在対応している行番号
- `source_text`: 該当行の元テキスト
- `present_in_todo`: 今回の同期結果として `TODO.md` に存在するか
- `attempt_count`: run を開始した回数
- `latest_run_id`: 直近 run id
- `last_error`: 直近失敗の短い説明。未失敗なら `null`
- `created_at`: 初回作成時刻
- `updated_at`: 最終更新時刻

### 8.4 Phase 2 で入れないフィールド

以下は将来候補だが、この Phase では入れない。

- `description`
- `priority`
- `depends_on`
- `approval_required`
- `generated_by`
- `related_paths`

理由は、まだ `TODO.md` から安定して抽出できず、空欄だらけの schema を先に固定しない方がよいためである。

## 9. `project state` 仕様

`runtime/state/project.json` は最低限以下を持つ。

```json
{
  "project": "demo",
  "status": "active",
  "active_task_id": null,
  "last_run_id": "run-20260314-120000-L3",
  "last_run_at": "2026-03-14T12:04:12+09:00",
  "task_counts": {
    "todo": 1,
    "in_progress": 0,
    "done": 2,
    "failed": 0
  },
  "updated_at": "2026-03-14T12:04:12+09:00"
}
```

### 9.1 `project status`

Phase 2 では `status` は `"active"` 固定でよい。

将来的に以下へ広げられる余地だけ残す。

- `active`
- `blocked`
- `idle`
- `archived`

### 9.2 `task_counts`

`task_counts` は `present_in_todo == true` の task を対象に集計する。

理由:

- 人間が今見ている `TODO.md` と整合する数字にしたい
- 消えた historical task まで数えるとノイズが増える

## 10. event log 仕様

### 10.1 ファイル

event log は以下に保存する。

```text
runtime/events/events.jsonl
```

1 event = 1 行の JSON とし、追記のみを行う。

### 10.2 共通フィールド

各 event は最低限以下を持つ。

```json
{
  "id": "evt-20260314-120000-0001",
  "timestamp": "2026-03-14T12:00:00+09:00",
  "project": "demo",
  "task_id": "task-0001",
  "run_id": "run-20260314-120000-L3",
  "type": "run.started",
  "actor": "ap.start",
  "payload": {
    "agent": "claude"
  }
}
```

### 10.3 event id

event id は以下の性質を満たせばよい。

- project 内で実用上ユニーク
- timestamp からおおよその並びが読める

推奨形式:

```text
evt-YYYYMMDD-HHMMSS-SSSS
```

末尾 `SSSS` は同一プロセス内の連番でよい。

### 10.4 Phase 2 の最小 event type

Phase 2 では以下だけ実装すればよい。

- `task.discovered`
- `task.selected`
- `task.status_changed`
- `run.started`
- `run.finished`
- `todo.sync_conflict`

### 10.5 event の意味

- `task.discovered`: `TODO.md` 同期で task を新規作成した
- `task.selected`: 今回の `ap start` で対象 task に選んだ
- `task.status_changed`: task status が変化した
- `run.started`: child process を起動した
- `run.finished`: child process が終了した
- `todo.sync_conflict`: `TODO.md` 更新や同期で衝突を検出した

### 10.6 stdout/stderr event をまだ入れない理由

`PLAN.md` には `run.stdout` / `run.stderr` もあるが、Phase 2 では入れない。

理由:

- まずは高シグナルな履歴だけで十分
- 詳細ログは既に `runtime/runs/*/*.log` にある
- JSONL に大量追記すると実装・検証コストが上がる

## 11. run artifact の拡張

Phase 1 の run directory 構造は維持しつつ、`meta.json` と `result.json` に task state と結び付く情報を追加する。

### 11.1 `meta.json`

最低限、以下の追加を推奨する。

```json
{
  "run_id": "run-20260314-120000-L3",
  "task_id": "task-0001",
  "attempt_number": 1,
  "project": "demo",
  "task_title": "first task",
  "status": "running"
}
```

### 11.2 `result.json`

最低限、以下の追加を推奨する。

```json
{
  "run_id": "run-20260314-120000-L3",
  "task_id": "task-0001",
  "attempt_number": 1,
  "status": "succeeded",
  "final_task_status": "done",
  "todo_update_applied": true
}
```

### 11.3 `last_run.json`

`runtime/last_run.json` は互換維持のため残す。

ただし役割は以下へ限定する。

- 直近 run の簡易参照
- 既存テスト互換

長期的には `project.json` がより正本に近い位置づけになる。

## 12. `ap start` の実行フロー変更

Phase 2 の `ap start` は以下の順で処理する。

1. project 解決
2. managed path 解決
3. `runtime/events`, `runtime/state/tasks`, `runtime/runs` を作成
4. `TODO.md` を読み、task state と同期
5. `project.json` を更新
6. state から runnable task を 1 件選択
7. 選んだ task を `in_progress` に更新し、`attempt_count` を増やす
8. `task.selected`, `task.status_changed`, `run.started` を追記
9. agent を実行し、run artifact を保存
10. 成功なら task status を `done` に更新する
11. 失敗なら task status を `failed` に更新する
12. 成功時は可能なら `TODO.md` を `[x]` に更新する
13. `run.finished` と必要な `task.status_changed` を追記
14. `project.json` と `last_run.json` を更新して終了

## 13. task 選択ルール

同期後の task 選択は以下で行う。

1. `present_in_todo == true`
2. `status == "todo"` または `status == "failed"`
3. `TODO.md` 上の出現順で最初

この Phase では priority/dependency は導入しない。

### 13.1 `failed` task の扱い

retry policy はまだ導入しないが、Phase 2 では `failed` task も再度選択対象に含めてよい。

理由:

- まだ retry 回数上限や blocker 判定がない
- 最小構成では「未完了 task は再度試せる」で十分

## 14. status 遷移

Phase 2 で許可する主な遷移は以下。

- `todo -> in_progress`
- `in_progress -> done`
- `in_progress -> failed`
- `failed -> in_progress`
- `done -> todo` (`TODO.md` で再オープンされた場合)
- `todo -> done` (`TODO.md` で手動完了された場合)
- `failed -> done` (`TODO.md` で手動完了された場合)

不正な遷移を厳密に一般化する必要はまだないが、少なくとも `task.status_changed` を記録し、`from` と `to` を payload に残す。

## 15. `TODO.md` 更新ルール

### 15.1 成功時

agent exit code が `0` の場合、可能なら該当 task 行を `[x]` に更新する。

### 15.2 `TODO.md` 更新に失敗した場合

Phase 1 からの重要な変更点として、更新失敗時も task state は `done` にしてよい。

ただし以下を行う。

- `result.json` の `todo_update_applied` を `false` にする
- `todo.sync_conflict` event を残す
- `last_error` は更新しない

理由:

- Phase 2 では state が機械向け truth だから
- `TODO.md` は人間向け投影であり、常に同期成功するとは限らないから

### 15.3 衝突検出

更新時は少なくとも以下を確認する。

- `source_line` がまだ存在する
- 行内容が `source_text` と一致する

一致しない場合は `TODO.md` を変更しない。

## 16. エラー仕様

既存の Phase 1 エラーに加えて、少なくとも以下を考慮する。

- `runtime/state/tasks` の読み書き失敗
- `runtime/events/events.jsonl` への追記失敗
- task state JSON の破損
- duplicate task title により同期不能

stderr メッセージ例:

```text
ap start failed: duplicate TODO task titles are not supported in Phase 2
ap start failed: failed to read task state: /abs/path/to/task-0001.json
ap start failed: failed to append event log
```

細かい終了コード分離はまだ不要で、終了コードは引き続き `0` / `1` でよい。

## 17. 並行実行の扱い

Phase 2 でも lock は導入しない。

そのため仕様として以下を維持する。

- 同一 project の同時 `ap start` は未サポート
- `events.jsonl` 追記競合時の挙動は未定義
- lock/retry/idempotency は Phase 5 で導入する

## 18. 推奨実装単位

既存実装へ無理なく載せるなら、責務は以下に分けるのがよい。

- `src/commands/cmd_start.cpp`
  - 全体 orchestration
- `src/projects/todo_task_selector.cpp`
  - checklist 解析の再利用
- `src/projects/todo_task_sync.cpp`
  - `TODO.md` と task state の同期
- `src/runtime/task_state_store.cpp`
  - task/project state の読み書き
- `src/runtime/event_log.cpp`
  - JSONL append

対応する header も `include/autopilot/...` に追加する。

この repo は外部 JSON library をまだ持っていないため、Phase 2 では以下を推奨する。

- JSON schema を単純に保つ
- scalar 中心の構造にする
- 最低限の手書き serializer/parser で済ませる

## 19. テスト観点

既存の `tests/test_ap_start.sh` を拡張する形で、少なくとも以下をカバーする。

- 初回実行で `runtime/state/project.json` が作られる
- 初回実行で `runtime/state/tasks/task-0001.json` が作られる
- 初回実行で `runtime/events/events.jsonl` が作られる
- 成功時に task state が `done` になる
- 失敗時に task state が `failed` になる
- 再実行時に `attempt_count` が増える
- `latest_run_id` が更新される
- `project.json` の `last_run_id` と `task_counts` が更新される
- `TODO.md` に `[x]` がある task を state sync で `done` と認識できる
- `TODO.md` で `done` task を `[ ]` に戻すと `todo` に戻る
- task 行が消えた場合に `present_in_todo: false` になる
- 成功したが `TODO.md` 更新が競合した場合、task state は `done` のままで `todo_update_applied` は `false` になる
- event log に `task.selected`, `run.started`, `run.finished` が残る
- duplicate title の未完了 task があると失敗する

テストは引き続き fake agent script を使い、実 agent 呼び出しに依存しない。

## 20. Phase 3 以降への引き継ぎ

Phase 2 が完了すると、次の Phase はかなり進めやすくなる。

- Phase 3 では event type を増やせる
- Phase 4 では task schema に priority/dependency/provenance を追加できる
- Phase 5 では `attempt_count` と `latest_run_id` を使って retry/lock を導入できる
- Phase 6 では reviewer run を task/run/event モデルへ自然に追加できる

重要なのは、ここで以下が揃うことである。

- task ごとの内部 ID
- project/task の現在状態
- run との関連付け
- append-only の履歴

## 21. 未解決事項

Phase 2 着手の blocker ではないが、実装前に意識しておくべき点がある。

- task state JSON の parser をどこまで汎用にするか
- duplicate title 制約を仕様として受け入れるか、別の task 識別子を導入するか
- `TODO.md` 手動編集をどこまで積極的に state へ反映するか
- `project.json` に schema version を入れるか

現時点の推奨は以下。

- Phase 2 では schema version を省略してよい
- duplicate title は明示的に非対応とする
- state/event/runs の整合を先に固める

この方針なら、Phase 1 の単純さを大きく崩さずに、`autopilot` を task/state/event 中心の実行基盤へ移行できる。
