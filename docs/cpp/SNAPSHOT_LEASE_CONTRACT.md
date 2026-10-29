# قرارداد snapshot و lease برای اسکن طولانی — COR-002

تاریخ: ۲۰۲۶-۱۰-۲۸. وابسته به [قرارداد pagination](PAGINATION_CONTRACT.md).
این قرارداد native، stream پایتون، خروجی HTTP/NDJSON و full validation در lab را تفکیک می‌کند؛
اسکن طولانی به معنی snapshot زندهٔ نامحدود بدون مصرف منابع نیست.

## native: idle lease، نه عمر مطلق پنج‌دقیقه‌ای

- اولین `FindPage` یک snapshot ثابت می‌گیرد؛ تمام continuationها همان snapshot و condition اولیه را مصرف می‌کنند.
  نوشتن پس از صفحهٔ نخست وارد این اسکن نمی‌شود. token فقط برای همان موتور باز و collection معتبر است؛
  پس از restart، close یا expiry قابل استفاده نیست. token مجوز امنیتی نیست؛ سرویس باید authorization خود را اعمال کند.
- idle lease برابر **۱۲۰ ثانیه** است. در مرز `now >= expires_at` درخواست بعدی رد می‌شود.
  ساعت production از `steady_clock` است، نه ساعت دیواری؛ ساعت ثابت قابل تزریق فقط از friend تست C++ در دسترس است، نه API کاربر.
- **عمر مطلق native عمداً ندارد**: اسکن پیش‌رونده می‌تواند از پنج دقیقه یا چند ساعت عبور کند.
  شروع درخواست معتبر، snapshot را برای همان call نگه می‌دارد؛ reaper حین اجرای آن snapshot را آزاد نمی‌کند.
  پس از تولید موفق صفحه، lease از زمان پایان کار تمدید می‌شود؛ صفحهٔ کند سهم idle بعدی را مصرف نمی‌کند.
- زمان پردازش/انتقال صفحه در caller نیز idle محسوب می‌شود: اگر پیش از درخواست صفحهٔ بعد ۱۲۰ ثانیه بگذرد،
  continuation خطا می‌دهد. ارسال heartbeat برای نامحدودکردن نگهداری snapshot یا تمدید از بیرون نداریم.
- reaper مستقل تقریباً هر ۱۰۰ms session بیکار منقضی را آزاد می‌کند؛ scheduling می‌تواند این آزادسازی را عقب بیندازد،
  اما اعتبار token هنگام درخواست به ساعت چک می‌شود. `MemoryReport` و شروع page نیز بیکارهای منقضی را پاک می‌کنند.
- پایان طبیعی، خطای storage/decode/oversized document، close و shutdown مالکیت registry را آزاد می‌کنند.
  `ClosePage` تکرارپذیر و بی‌خطر برای token نامعتبر است. caller باید پیش از نابودی DocEngine درخواست‌های درحال‌اجرا را drain کند.
  shutdown هم‌زمان با destructor و call فعال مجاز نیست؛ `PrepareShutdown` جایگزین drain نیست.
- close در میانهٔ call snapshot را زیر پای آن آزاد نمی‌کند؛ اگر call به continuation نیاز داشته باشد،
  با خطای `Page session closed during scan` برمی‌گردد و session را احیا نمی‌کند. صفحهٔ نهاییِ کامل ممکن است پیش از اثر close تمام شود.

## منابع و مشاهده‌پذیری

سقف **۱۲۸ snapshot صفحهٔ زنده** در سطح موتور اعمال می‌شود، نه فقط تعداد tokenهای ثبت‌شده.
snapshot بسته‌شده ولی درحال‌استفاده نیز تا آخرین release ظرفیت مصرف می‌کند. ظرفیت جدید با خطا رد می‌شود؛
cursor فعال برای بازکردن جا silently حذف نمی‌شود. هر درخواست فقط صفحهٔ bounded قرارداد COR-001 را می‌سازد؛
iterator صفحه پس از پایان call از بین می‌رود و scan با `fill_cache=false` cache را عمداً پر نمی‌کند.

`engine.memory_report()` شامل موارد زیر است:

| فیلد | معنا |
| --- | --- |
| `page_sessions` / `page_session_capacity` | sessionهای registry و سقف snapshotهای صفحه |
| `page_live_snapshots` | همهٔ snapshotهای صفحه، شامل retired/in-flight |
| `page_retired_inflight_snapshots` | از registry خارج شده ولی هنوز نگه داشته‌شده |
| `page_active_sessions` / `page_idle_sessions` | registry دارای borrower / بدون borrower؛ queued call نیز borrower است |
| `page_oldest_session_age_seconds` | بیشترین عمر monotonic session ثبت‌شده |
| `page_idle_lease_seconds` | ۱۲۰ |
| `page_absolute_lifetime_seconds` | `null`: native سقف مطلق ندارد |
| `page_snapshot_bytes` | `null`: نسبت‌دادن دقیق بایت به هر snapshot در دسترس نیست |

فیلدهای موجود `rocksdb_snapshots`، `oldest_snapshot_unix_seconds`، `cache_pinned_bytes` و
`retained_memtables_bytes` نیز مشاهده‌های سراسری RocksDB هستند، نه جمع قابل انتساب به این sessionها.
گزارش sampled است، نه snapshot اتمیک همهٔ counters. **۱۲۸ سقف تعداد است، نه تضمین سقف RSS یا فضای دیسک**؛
snapshot قدیمی می‌تواند تاریخچهٔ نسخه‌ها را از compaction محافظت کند. بودجهٔ engine/cgroup و نظارت دیسک
همچنان لازم است. hard byte-quota به‌ازای snapshot و لغو call گیرکرده در storage در این تسک ادعا نمی‌شود.

## stream، export و validation: شکست ناقص موفق نیست

- `iter_documents` تنبل است؛ snapshot در اولین دریافت page ساخته می‌شود، نه صرف ساخت object.
  از `with`/`close()` استفاده کنید. native error به `RuntimeError` تبدیل می‌شود، نه EOF موفق؛
  caller نباید آن را بگیرد و سپس خروجی جزئی را کامل اعلام کند. دادهٔ صفحهٔ خطادار قابل قبول نیست.
- `FindPage.success=false` همراه `[]` یعنی شکست، نه مجموعهٔ خالی موفق. هیچ restart خودکار روی snapshot تازه انجام نمی‌شود.
  پس از expiry خروجی قبلی را ناقص بدانید و job تازه را از ابتدا اجرا کنید.
- export فعلی همان streamهای document/query با NDJSON است؛ endpoint جدید export یا export نامحدود ساخته نشده است.
  HTTP علاوه بر lease native، **deadline مطلق ۱۲۰ ثانیه‌ای response**، quota پیش‌فرض ۸ cursor کل/۲ برای هر owner،
  backpressure و authorization بین batchها دارد. حذف سقف native این حفاظت HTTP را حذف نمی‌کند.
  پس از ارسال header، HTTP 200 به‌تنهایی اثبات موفقیت نیست: فقط `{"end":true}` پایان موفق است.
  خطای میانی `{"error":...}` بدون end یا قطع transport است؛ driver هر دو را رد می‌کند.
  مصرف‌کنندهٔ export فایل را موقت بنویسد و فقط پس از end معتبر منتشر کند؛ فایل جزئی را خروجی نهایی نداند.
- اسکن بزرگ/طولانی فعلاً از native/lab انجام می‌شود؛ export چندساعتهٔ HTTP نیازمند طراحی job و artifact جداگانه است،
  نه بالا بردن پنهان deadline تمام درخواست‌ها.
- `NexoraAdapter.iter_records` خطا را عبور می‌دهد و stream را در `finally` می‌بندد.
  `full_validate` فقط پس از EOF صحیح، شمارش دقیق و oracle/fingerprint موفق گزارش می‌دهد؛ expiry پس از یک سند هم آن را fail می‌کند.

## شواهد و بازتولید کوتاه

تست‌های C++ با ساعت تزریقی، بدون انتظار واقعی چند دقیقه/چند ساعت:

- عبور اسکن ثابت از پنج دقیقه با صفحه‌های پیش‌رونده و عدم مشاهدهٔ نوشتن تازه؛
- پذیرش در ۱۱۹ ثانیه، تمدید، رد دقیق در ۱۲۰ ثانیه و پاک‌سازی مستقل reaper؛
- صفحهٔ متوقف‌شده روی mutex بیش از lease، تمدید پس از پایان، و close هم‌زمان بدون resurrection؛
- ۱۲۸ snapshot، snapshot retired/in-flight و آزادشدن ظرفیت پس از آخرین release؛
- native Python: session لغوشده خطاست نه پایان موفق؛ export و driver: خطای میانی بدون end؛ lab: validation ناقص رد و stream بسته می‌شود.

از root در WSL:

```bash
cmake --build build/release --target nexoradb_py nexora_doc_tests nexora_research_read_probe -j2
cmake -E copy_if_different nexoradb.cpython-310-x86_64-linux-gnu.so NexoraDB/src/nexoradb/native/nexoradb.cpython-310-x86_64-linux-gnu.so
build/release/tests/cpp/nexora_doc_tests
build/release-0.4.1-install-test/bin/python -m unittest discover -s tests -p test_pagination_contract.py -v
PYTHONPATH=. nexora-scale-lab/.venv/bin/python -m pytest -q tests/test_cursor_pool_acl.py tests/test_api_capacity_streaming.py tests/test_memory_reporting.py tests/test_native_threading_contract.py
PYTHONPATH=nexora-scale-lab nexora-scale-lab/.venv/bin/python -m unittest discover -s nexora-scale-lab/tests -v
```

تست مقیاس ۱۰۰M یا soak چندساعته در این پذیرش اجرا نمی‌شود؛ پذیرش این تسک مربوط به قرارداد lifecycle و صحت است،
نه اثبات رفتار منابع تحت هر workload. داده‌ها و نتایج تاریخی بازنویسی نشده‌اند؛ هیچ commit انجام نشده است.

### نتیجهٔ اجرا در همین workspace

- commit پایه: `3930b5ada1b7b3a715acaeacbe3c62b11af0c8a3`، working tree دارای تغییرات مرحلهٔ صفر/COR-001 و این تسک است؛ commit تازه ساخته نشد.
- Release core، binding و research probe rebuild شدند؛ native ریشه و package SHA یکسان دارند:
  `da34d350827feb7c8e0a3cf2dbb11a1ad79e4b09d7cdecbaac405619b7eb6865`.
- native: **۶۴/۶۴ موفق**، شامل ۹ pagination؛ Python pagination: **۶/۶ موفق**.
- API/cursor ACL/memory/threading: **۲۵/۲۵ موفق**؛ فقط هشدار deprecation موجود Starlette/httpx.
- lab: **۶۰ تست، ۵۳ موفق و ۷ skip اختیاری peer integration**؛ research با opt-inهای native/TCP/cgroup: **۲۸/۲۸ موفق**.
- checker نسخه: **۰٫۴٫۱** همخوان؛ نتایج تاریخی و شواهد A/A به binary جدید منتسب نشدند.
- RES-004/005 همچنان منتظر اجرای بلند کاربرند؛ این تست‌های کوتاه آن والدها را خودکار تکمیل نمی‌کنند.
