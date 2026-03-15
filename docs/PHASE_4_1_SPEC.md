# Phase 4.1 設計メモ: TODO.md への stable task ID 導入

## 1. 目的

Phase 4 で導入した task state 管理に対して、`TODO.md` 側にも stable task ID を導入し、TODO 同期時の task identity の曖昧さを解消する。

特に以下の問題を解決対象とする。

- TODO.md の並び替えで task identity が入れ替わる
- title ベース再結合により metadata が誤って別 task に付着する
- rename と reorder が同時に起きたときに同期が曖昧になる

ただし、引き続き **TODO.md は source of truth ではない**。  
task の正本は runtime/state 側にあり、TODO.md は **人間向けの投影 + 軽量編集インターフェース** として扱う。

---

## 2. この Phase の設計判断

## 2.1 TODO.md に visible task ID を埋め込む

各 TODO 行は visible な task ID を持つ。

例:

```md
- [ ] [demo-0001] Add ap start command
- [x] [demo-0002] Add runtime/state task persistence
````

task ID は各 project の `slug` を prefix に持つ。

形式:

```text
<slug>-0001
<slug>-0002
...
```

例:

* `demo-0001`
* `myproj-0042`

---

## 2.2 project.yaml に slug を導入する

各 project は `project.yaml` に stable な `slug` を持つ。

例:

```yaml
name: Demo Project
slug: demo
paths:
  - main
```

### 制約

* `slug` は project 内 task ID の prefix である
* `slug` は project 内で不変とする
* `slug` は人間が `ap new` 時に指定する
* `slug` の文字種は制限する

推奨制約:

* 英小文字
* 数字
* ハイフン
* 先頭は英字
* 例: `demo`, `web-app`, `autopilot`

正規表現例:

```text
^[a-z][a-z0-9-]*$
```

---

## 2.3 task identity の主キーは TODO 行ではなく task ID

TODO 同期は line number や title を主キーにせず、**task ID を主キー**として行う。

重要ルール:

* ID がある行は、その ID の task に必ず紐づく
* title が変わっていても別 task とはみなさない
* line の並び替えは identity に影響しない
* title は表示項目であり、identity 判定には使わない

---

## 2.4 TODO.md は source of truth ではない

この Phase でも以下は維持する。

* task の正本は runtime/state 側
* TODO.md は人間向けの一覧
* TODO.md は一部編集可能だが、全 metadata の正本ではない

このため、TODO.md と task state の間にある程度の drift は許容する。
ただし、**identity conflict は許容しない**。

---

## 3. TODO.md で編集可能とみなす項目

TODO.md は軽量 task list として扱う。
人間が編集してよいのは以下に限定する。

## 3.1 編集可能

* 行の並び順
* チェック状態
* title
* 新規行追加
* 行削除

## 3.2 TODO.md を正本にしない項目

以下は TODO.md の正本化対象外とする。

* priority
* depends_on
* approval_required
* retry_count
* run 状態
* 履歴
* related_paths
* latest_run_id
* blocked reason
* generated_by
* その他 runtime/state に属する metadata

これらは task state 側で管理する。

---

## 4. TODO 行フォーマット

## 4.1 基本形式

```md
- [ ] [demo-0001] Add ap start command
- [x] [demo-0002] Persist task state to runtime/state
```

要素:

* checkbox
* visible task ID
* title

## 4.2 パース対象

最低限パース対象とするのは以下。

* checked / unchecked
* task ID
* title

## 4.3 パース要件

* task ID は必須
* task ID 形式が不正な行はエラー
* 同一 TODO.md 内で task ID 重複は hard error
* title が空はエラー
* checkbox が壊れている行はエラー

---

## 5. task ID の生成規則

## 5.1 採番形式

各 project の task ID は `<slug>-NNNN` 形式とする。

例:

* `demo-0001`
* `demo-0002`
* `demo-0003`

初期実装では 4 桁固定とする。

## 5.2 採番主体

task ID の採番は CLI が行う。
主経路は以下。

* `ap task add`
* 将来の task 自動生成
* 必要に応じた import 処理

## 5.3 人間の手編集について

人間が TODO.md を直接編集して新規行を追加すること自体は許容する。
ただし、以下の扱いを明確にする。

* ID 付き新規行: 既存 task として解釈を試みる
* ID なし新規行: 自動 import 対象またはエラー

Phase 4.1 では設計を単純に保つため、**TODO.md の新規行は基本的に CLI 経由で追加する運用を推奨**する。
直接手編集で新規行を追加する場合の扱いは、次のどちらかで実装する。

### 選択肢A

ID なし新規行は hard error

### 選択肢B

ID なし新規行は sync 時に新規 task として採番して取り込む

Phase 4.1 では安全性優先で **選択肢A を推奨**する。
つまり、TODO.md に新規 task を追加する場合も `ap task add` を基本経路とする。

---

## 6. drift の扱い

## 6.1 許容する drift

以下は許容する。

* TODO.md と state の title の微差
* TODO.md の並び順差
* TODO.md に未反映の metadata
* state にだけ存在する補助情報
* state にだけ残る completed/archived task

## 6.2 許容しない drift

以下は hard error または要修正とする。

* 同じ TODO.md 内で同一 ID が複数回現れる
* 存在しない ID を既存 task として黙って別 task に再結合する
* 1 つの ID を別 task の identity として扱う
* ID 形式不正
* project slug と一致しない task ID prefix
* ID conflict を title や line 番号で黙って解決すること

## 6.3 設計原則

* **表示の不整合は寛容**
* **identity の不整合は厳密**

---

## 7. TODO 同期ポリシー

## 7.1 TODO -> state

### ケース1: 行に ID があり、state にその task が存在する

* 同一 task とみなす
* title 変更は rename として扱う
* checkbox 変化は state へ反映可能
* metadata は state 優先

### ケース2: 行に ID があり、state にその task が存在しない

* 原則としてエラー
* 黙って別 task に再結合しない
* 将来 import 専用フローがある場合のみ明示的に処理

### ケース3: 行に ID がない

* Phase 4.1 では原則エラー
* `ap task add` 使用を促す

### ケース4: 同一 ID が複数行にある

* hard error

### ケース5: ID の slug prefix が project と一致しない

* hard error

---

## 7.2 state -> TODO

TODO.md は state から再描画可能である。

基本方針:

* 各 task は ID 付きで投影する
* title は state を基準に描画してよい
* 表示対象 task は task status に応じて制御可能
* default では active task を主に表示する

表示対象の例:

* `todo`
* `ready`
* `in_progress`
* `review_pending`
* `blocked`

`done` や `cancelled` を表示するかは将来設定可能にしてよい。

---

## 7.3 conflict 解決

### title 不一致

* 同一 ID であれば同一 task
* title 差分は rename または drift として扱う
* identity conflict にはしない

### metadata 不一致

* state 優先
* TODO.md の metadata 非保持項目は TODO 側から復元しない

### identity conflict

* fail-fast
* 自動修復しない
* title や line 番号からの推定再結合をしない

---

## 8. CLI 方針

## 8.1 `ap new`

`ap new` 時に `slug` を必須入力として受け取る。

生成物:

* `project.yaml`
* `TODO.md`
* `dashboard.md`

`project.yaml` に `slug` を保存する。

## 8.2 `ap task add`

新規 task は原則 `ap task add` で登録する。

責務:

* task ID 採番
* task state 生成
* TODO.md 更新
* 必要に応じた event 記録

## 8.3 TODO 手編集の位置付け

TODO.md の手編集は補助経路であり、主経路ではない。

* rename
* checkbox 更新
* 並び替え

程度は許容するが、新規 task 追加の主経路は CLI とする。

---

## 9. 実装への影響

## 9.1 project 設定

* `project.yaml` に `slug` を追加
* `ap new` で slug を受け付ける
* slug validation を導入する

## 9.2 TODO parser

* TODO 行から task ID を抽出する
* ID 形式検証を行う
* duplicate ID を検出する
* project slug との整合を検証する

## 9.3 TODO sync

既存の line/title 推定中心の再結合から、**ID 中心同期**へ変更する。

新しい優先順位:

1. task ID 一致
2. それ以外は再結合しない

つまり、title や line は補助情報であって identity の主キーではない。

## 9.4 task state store

task state は既存どおり正本として保持する。
TODO 側の変更で task identity を作り替えない。

## 9.5 ap start

task 選択は state を正本として行う。
TODO.md は補助入力にすぎず、選択ロジックの正本ではない。

---

## 10. 非目標

Phase 4.1 では以下は扱わない。

* 既存 project の自動 migration
* TODO.md の ID 自動埋め込み
* TODO.md の自由文からの高度な task import
* visible ID を隠す UI 改善
* task ID のリネーム
* project slug の変更対応

既存 project は手動修正とする。

---

## 11. バリデーション方針

以下は fail-fast とする。

* `project.yaml` の slug 不正
* TODO.md の task ID 不正
* TODO.md の duplicate ID
* project slug と task ID prefix の不一致
* state に存在しない task ID を TODO が参照
* TODO 行フォーマット破損

エラーメッセージは、対象ファイル・行番号・ID を含めること。

例:

```text
failed to parse TODO.md: duplicate task id demo-0003 at line 12
failed to parse TODO.md: invalid task id format [Demo-3] at line 4
failed to sync TODO.md: task id web-0007 does not exist in state
failed to read project.yaml: invalid slug "Demo Project"
```

---

## 12. テスト観点

最低限追加すべきテスト:

1. TODO 並び替え後も ID により同一 task が維持される
2. TODO title 変更が rename として扱われる
3. duplicate ID を reject する
4. invalid task ID format を reject する
5. project slug と task ID prefix 不一致を reject する
6. state に存在しない task ID を reject する
7. `ap task add` で `<slug>-0001` 形式の ID が採番される
8. `ap new` で slug を必須入力にできる
9. TODO の metadata 非保持項目が state 優先で維持される
10. `ap start` が title ではなく state/ID に基づいて正しく task 選択する

---

## 13. 受け入れ基準

Phase 4.1 の完了条件は以下。

* `project.yaml` に slug を持てる
* `ap new` で slug を作成できる
* TODO.md に visible task ID を表示できる
* TODO sync が ID ベースで動く
* title 変更や並び替えで identity が崩れない
* duplicate ID / invalid ID / unknown ID を fail-fast できる
* TODO.md を source of truth にしない方針が維持されている

---

## 14. 最終方針まとめ

Phase 4.1 では、TODO.md に visible task ID を導入し、TODO 同期の主キーを title/line ではなく task ID に切り替える。

ただし、引き続き TODO.md は source of truth ではない。

整理すると以下になる。

* task identity の正本は runtime/state
* TODO.md は人間向け投影
* TODO 行は stable task ID を持つ
* title が変わっても ID が同じなら同一 task
* line の並び替えは identity に影響しない
* 表示の不整合は許容する
* identity conflict は hard error にする
* 新規 task の主経路は `ap task add`
* 既存 project の migration は行わず手動修正とする

この方針により、TODO.md を唯一の truth にしないまま、Phase 4 で顕在化した identity 曖昧性を大幅に減らせる。
