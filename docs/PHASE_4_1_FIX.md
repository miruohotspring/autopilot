# Phase 4.1 修正計画メモ

## 1. 背景

Phase 4.1 で `TODO.md` に visible task ID を導入し、TODO 同期の主キーを title / line ではなく task ID に切り替えた。

その後の実装レビューで、以下の問題が確認された。

1. `ap task add` が state 保存前に `TODO.md` を更新しており、途中失敗で project を壊す
2. path 未登録 project で作成した task が `related_paths=["main"]` 固定となり、後から別名 path を追加すると `ap start` できなくなる
3. `ap task add` が unknown TODO ID を fail-fast せず、synthetic task を差し込んで採番を進めてしまう

この文書は、それらを修正するための方針を明文化する。

---

## 2. この修正で守るべき原則

## 2.1 TODO.md は source of truth ではない
- task identity の正本は runtime/state
- TODO.md は人間向け投影 + 軽量編集インターフェース

## 2.2 identity conflict は fail-fast
- unknown ID
- duplicate ID
- invalid ID
- slug prefix mismatch

は黙って吸収せず、明示的に失敗させる。

## 2.3 partial write を残さない
`ap task add` の失敗で、TODO だけ更新される / state だけ更新される、のような中途半端な永続状態を残してはならない。

## 2.4 path 未登録は「未設定」で表現する
存在しない path 名を仮置きで保存してはならない。

---

## 3. 問題1: `ap task add` が途中失敗で project を壊す

## 3.1 現象
現在の実装では、`ap task add` が以下の順序で処理している。

1. `TODO.md` 更新
2. task state 保存

このため、2 が失敗すると:

- `TODO.md` には `[slug-NNNN]` 行が残る
- runtime/state に対応 task が存在しない
- 次回 `ap start` が unknown ID で hard error になる

結果として、1 回の追加失敗が project 全体の開始不能に繋がる。

## 3.2 方針
`ap task add` は以下の論理操作を 1 単位として扱う。

- project の整合性検証
- 新規 task state 作成
- TODO.md 更新
- 必要なら event 記録

この一連は partial write を残さないように実装する。

## 3.3 最低限の修正方針
実装順序を次に変更する。

1. project の TODO/state 整合性を検証
2. 新規 task state を temp/staging に書く
3. TODO.md を更新
4. TODO 更新成功後に task state を正式 commit する
5. commit 失敗時は TODO.md を元内容へ戻す
6. 必要に応じて event を記録

重要なのは、**新規 task state をいきなり正式 state として露出しない**こと、および **TODO 更新後に state commit が失敗した場合は TODO を必ず元に戻す経路を持つ**ことである。

少なくとも:

- TODO だけ残る
- state だけ残る

のどちらも防ぐ実装にする。

## 3.4 推奨実装
可能であれば各ファイル更新は temp file + atomic rename で行う。

例:
- `tasks/demo-0003.json.tmp` に書く
- flush / close
- rename で正式ファイルへ置換

TODO.md も同様に temp file 経由で更新する。

---

## 4. 問題2: path 未登録時の `related_paths=["main"]` 仮置き

## 4.1 現象
新規 project は path 未登録で始まる。  
この状態で `ap task add` すると、default related path として `"main"` が task state に保存される。

その後、最初の path を `ap add ... -n src` のように `main` 以外で追加すると、`ap start` が:

- invalid related path in task ...

で失敗する。

## 4.2 問題の本質
path が存在しない時点で `"main"` を保存するのは、**存在しない path 名を予約している**のと同じである。

これは設計上不自然であり、後から正当な path 追加を壊す。

## 4.3 方針
path 未登録時の related path は `"main"` のような仮値ではなく、**未設定**として表現する。

## 4.4 具体方針
次のいずれかで表現する。

### 案A
`related_paths: []`

### 案B
`related_paths` 自体を未設定 / omitted

どちらでもよいが、Phase 4.1 では扱いやすさを優先して **空配列**でよい。

## 4.5 runtime 全体での意味づけ
- `related_paths` が未設定または空配列 → path 制約なし
- `related_paths` に値あり → その path のみ対象

これにより、task 追加時に path が未登録でも、後から任意の path 名を追加できる。

この意味づけは `ap start` の選択ロジックだけでなく、少なくとも以下に反映する必要がある。

- task state 読み込み時の `related_paths` デフォルト補完
- task state validation
- runnable task 選択時の path matching

---

## 5. 問題3: unknown TODO ID を synthetic task で吸収している

## 5.1 現象
`TODO.md` に state に存在しない ID があっても、`ap task add` がそれを fail-fast せず、synthetic task を作って整合したように見せた上で次の採番を進めている。

例:
- TODO.md に `[demo-0009] ghost` だけ存在
- state に `demo-0009` が存在しない
- `ap task add` が `demo-0010` を追加して成功

## 5.2 問題の本質
これは Phase 4.1 で決めた次の方針に反する。

- unknown ID は hard error
- drift を黙って吸収しない
- identity conflict は厳密に扱う

synthetic task は convenience としては理解できるが、壊れた TODO/state をさらに悪化させる。

## 5.3 方針
`ap task add` 開始時に project 整合性を検証し、unknown TODO ID があれば即失敗させる。

## 5.4 具体ルール
以下のいずれかが見つかったら、**新規 task を追加せず即失敗**とする。

- duplicate task ID in TODO
- invalid task ID format
- task ID prefix mismatch
- TODO の task ID が state に存在しない
- TODO 行フォーマット破損

synthetic recovery は行わない。

---

## 6. `ap task add` の新しい処理フロー

`ap task add` は以下の順で処理する。

1. project 設定読み込み
2. `project.yaml` の `slug` 検証
3. TODO.md 読み込み・構文検証
4. task ID の重複/形式/slug prefix を検証
5. TODO に出現する全 ID が state に存在するか検証
6. 次の task ID を採番
7. 新規 task state を構築
8. task state を保存
9. TODO.md を更新
10. event 記録（必要なら）
11. 途中失敗時は rollback または partial write なしを保証

重要なのは、**追加前の整合性検証**と**永続更新の順序**である。

---

## 7. 仕様への追記

Phase 4.1 仕様に以下を追記する。

### 7.1 整合性前提
`ap task add` は project の TODO/state 整合性が取れていることを前提に実行される。  
不整合がある場合、新規 task を追加せず fail-fast する。

### 7.2 unknown TODO ID
TODO.md に記載された task ID が state に存在しない場合、`ap task add` は成功してはならない。  
synthetic task による暗黙修復は行わない。

### 7.3 path 未登録
path 未登録 project で作成された task の `related_paths` は未設定または空配列とし、存在しない仮 path 名を保存してはならない。

### 7.4 partial write 禁止
`ap task add` は失敗時に TODO.md と state の片側だけを更新した状態を残してはならない。

---

## 8. 実装方針

## 8.1 `cmd_task_add.cpp`
修正対象:
- 事前検証フェーズ追加
- state を staging してから TODO 更新後に commit する順序へ変更
- TODO 更新失敗時の cleanup
- state commit 失敗時の TODO rollback
- synthetic task 生成削除
- default related path ロジック見直し

## 8.2 related path 決定ロジック
現在の `default_related_path_name()` 相当の処理は次のように変更する。

- path が 1 つ以上登録済みなら、その default policy を適用
- path が 0 件なら `related_paths=[]`

Phase 4.1 時点では、path 未登録なら空配列で十分。

## 8.3 runtime 側の修正対象
`related_paths=[]` を許容するため、`cmd_task_add.cpp` だけでなく runtime 側も修正対象に含める。

対象:
- `task_state_store.cpp` の `related_paths` 読み込みデフォルト
- `cmd_start.cpp` の `validate_task_state(...)`
- `cmd_start.cpp` の runnable task path matching

空配列を invalid 扱いしたり、未設定を既存 path 名へ暗黙補完したりしないこと。

## 8.4 validator の独立
可能であれば、TODO/state 整合性検証を専用関数に切り出す。

例:
- `validate_project_todo_and_state_consistency(...)`

これにより、将来 `ap start` / `ap task add` / `ap task sync` で共通利用しやすくなる。

---

## 9. テスト追加方針

最低限、以下の回帰テストを追加する。

## 9.1 state 保存失敗時に TODO が汚れない
手順:
- state 保存先を書き込み不可にする
- `ap task add` 実行

期待:
- 非0終了
- TODO.md に新規 `[slug-NNNN]` 行が残らない
- state に新規 task が存在しない

## 9.2 path 未登録 task が後から path 追加で壊れない
手順:
1. path 未登録 project 作成
2. `ap task add`
3. 最初の path を `main` 以外の名前で追加
4. `ap start`

期待:
- `related_paths` は空または未設定
- `ap start` が invalid related path で落ちない

## 9.3 unknown TODO ID があると `ap task add` は失敗
手順:
- TODO.md に state にない `[slug-0009]` を手で書く
- `ap task add` 実行

期待:
- 非0終了
- synthetic recovery しない
- 次 ID を採番しない
- TODO/state に新規変更が入らない

## 9.4 正常時のみ TODO と state が両方更新される
手順:
- 健全 project で `ap task add`

期待:
- 同じ新規 task ID が state と TODO に現れる
- 中途半端な片側更新がない

---

## 10. 受け入れ基準

この修正の完了条件は以下。

- `ap task add` 失敗で project を壊さない
- TODO だけ更新される partial write がない
- path 未登録時に `related_paths=["main"]` のような仮値を保存しない
- unknown TODO ID があると `ap task add` は fail-fast する
- synthetic task による暗黙修復をしない
- 追加した回帰テストが通る
- 既存正常系の task add / start フローを壊さない

---

## 11. 最終まとめ

今回の修正では、Phase 4.1 の設計思想を実装に揃えることが目的である。

守るべきポイントは以下。

- TODO.md は source of truth ではない
- identity conflict は厳密に扱う
- unknown ID を黙って吸収しない
- path 未登録は「未設定」として扱う
- `ap task add` は partial write を残さない

要するに、**壊れた状態を自動的に取り繕って先へ進むのではなく、壊れていれば止まり、正常時だけ確実に追加する** 方向に揃える。
