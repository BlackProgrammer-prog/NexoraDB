# دستورهای اجرای NexoraDB برای ارائه

## 1. فعال‌سازی محیط مجازی

```bash
cd ~/NexoraDB
source NexoraDB/.venv/bin/activate
```

بررسی محیط:

```bash
which python
python --version
which nexoradb
```

خروج از محیط:

```bash
deactivate
```

اگر محیط مجازی هنوز ساخته نشده است:

```bash
cd ~/NexoraDB
python3.10 -m venv NexoraDB/.venv
source NexoraDB/.venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -e 'NexoraDB[dev]'
```

## 2. اجرای عادی دیتابیس

```bash
nexoradb run --host 127.0.0.1 --port 8000
```

آدرس‌ها:

```text
Backend:  http://127.0.0.1:8000
Dashboard: http://127.0.0.1:8000/admin/
Health:    http://127.0.0.1:8000/health
```

بررسی سلامت:

```bash
curl http://127.0.0.1:8000/health
```

توقف اجرای عادی:

```text
Ctrl + C
```

دستور جایگزین:

```bash
nexoradb server --host 127.0.0.1 --port 8000
```

## 3. اجرای development با reload خودکار

```bash
nexoradb run --host 127.0.0.1 --port 8000 --reload
```

## 4. اجرای دیتابیس به‌صورت سرویس پس‌زمینه

شروع سرویس:

```bash
nexoradb run --service --host 127.0.0.1 --port 8000
```

نمایش log:

```bash
nexoradb run --log
nexoradb run --log 200
```

توقف سرویس:

```bash
nexoradb run --stop
```

مسیر فایل‌های سرویس:

```text
~/.nexoradb/service.pid
~/.nexoradb/service.log
```

## 5. اجرای هم‌زمان Backend و Dashboard در حالت توسعه

```bash
nexoradb dev \
  --api-host 127.0.0.1 \
  --api-port 8000 \
  --dashboard-host 127.0.0.1 \
  --dashboard-port 5173
```

## 6. اجرای جداگانه Dashboard

حالت توسعه:

```bash
nexoradb dashboard --mode dev --host 127.0.0.1 --port 5173
```

حالت فایل‌های buildشده:

```bash
nexoradb dashboard --mode static --host 127.0.0.1 --port 5173
```

## 7. اجرای Textual CLI

```bash
nexoradb cli --url http://127.0.0.1:8000
```

## 8. اجرای Query مستقیم در ترمینال

ابتدا سرور را اجرا کنید:

```bash
nexoradb run --host 127.0.0.1 --port 8000
```

در ترمینال دوم login کنید و به‌جای `YOUR_PASSWORD` پسورد root را قرار دهید:

```bash
export NEXORA_TOKEN="$(
  curl -sS http://127.0.0.1:8000/auth/login \
    -H 'Content-Type: application/json' \
    -d '{"username":"root","password":"YOUR_PASSWORD"}' |
  python -c 'import json,sys; print(json.load(sys.stdin)["accessToken"])'
)"
```

اجرای Query ساده:

```bash
curl -sS http://127.0.0.1:8000/query/execute \
  -H "Authorization: Bearer $NEXORA_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"query":"SHOW GRAPHS;"}' |
python -m json.tool
```

بررسی گراف استاد:

```bash
curl -sS http://127.0.0.1:8000/query/execute \
  -H "Authorization: Bearer $NEXORA_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"query":"GRAPH STATUS professor_social;"}' |
python -m json.tool
```

اجرای الگوریتم:

```bash
curl -sS http://127.0.0.1:8000/query/execute \
  -H "Authorization: Bearer $NEXORA_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"query":"RUN LOCK FriendSuggestion ON professor_social WITH user='\''W2'\'', edge_type='\''FOLLOWS'\'' LIMIT 3;"}' |
python -m json.tool
```

## 9. ورود دیتای استاد

فایل ورودی:

```text
professor_social_import.nql
```

اجرای کامل فایل از ترمینال:

```bash
python -c '
import json
from pathlib import Path
print(json.dumps({
    "query": Path("professor_social_import.nql").read_text(encoding="utf-8")
}))
' |
curl -sS http://127.0.0.1:8000/query/execute \
  -H "Authorization: Bearer $NEXORA_TOKEN" \
  -H 'Content-Type: application/json' \
  --data-binary @- |
python -m json.tool
```

خروجی مورد انتظار:

```text
Graph: professor_social
Nodes: 100
Edges: 778
State: Clean
```

> فایل import را فقط یک‌بار اجرا کنید؛ اجرای دوباره با همان نام‌ها خطای وجود Collection و Graph می‌دهد.

## 10. اجرای تست دیتای استاد

```bash
NexoraDB/.venv/bin/python -m pytest -q -s \
  tests/test_professor_dataset_algorithms_e2e.py
```

خروجی مورد انتظار:

```text
12 passed
```

## 11. اجرای تست استاد و تست مرجع الگوریتم‌ها

```bash
NexoraDB/.venv/bin/python -m pytest -q -s \
  tests/test_professor_dataset_algorithms_e2e.py \
  tests/test_query_algorithms_e2e.py
```

خروجی مورد انتظار:

```text
25 passed
```

## 12. اجرای فقط تست FriendSuggestion

```bash
NexoraDB/.venv/bin/python -m pytest -q -s \
  tests/test_professor_dataset_algorithms_e2e.py::ProfessorDatasetAlgorithmTests::test_friend_suggestion_counts_each_mutual_friend_once
```

## 13. rebuild افزونه Native پس از تغییر C++

```bash
cmake --build cmake-build-debug --target nexoradb_py -j2
```

کپی باینری داخل package:

```bash
cmake -E copy_if_different \
  nexoradb.cpython-310-x86_64-linux-gnu.so \
  NexoraDB/src/nexoradb/native/nexoradb.cpython-310-x86_64-linux-gnu.so
```

سپس سرور را restart کنید:

```bash
nexoradb run --stop
nexoradb run --service --host 127.0.0.1 --port 8000
```
