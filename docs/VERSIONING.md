# مدیریت متمرکز نسخهٔ NexoraDB

مرجع نسخهٔ جاری: `NexoraDB/src/nexoradb/_version.py`.
برای تغییر نسخه تنها یک فرمان از root مخزن کافی است:

```powershell
wsl -d Ubuntu -- python3 /home/parham/NexoraDB/scripts/release_version.py --set 0.4.0
```

یا مقدار `__version__` را در فایل مرجع تغییر دهید و اجرا کنید:

```bash
python3 scripts/release_version.py --sync
python3 scripts/check_release_version.py
```

بررسی بدون نوشتن:

```bash
python3 scripts/release_version.py --check
```

| محل | رفتار |
|---|---|
| `NexoraDB/src/nexoradb/_version.py` | مرجع اصلی؛ اسکریپت آن را import نمی‌کند. |
| `CMakeLists.txt` | project version همگام؛ build بعدی نسخهٔ native و `build_info()` را می‌سازد. |
| `NexoraDB/pyproject.toml` | نسخهٔ wheel و metadata بسته همگام می‌شود. |
| `vcpkg.json` | نسخهٔ خود پروژه همگام؛ نسخهٔ dependencyها تغییر نمی‌کند. |
| `frontend/package.json` | نسخهٔ frontend همگام می‌شود. |
| `frontend/package-lock.json` | نسخهٔ ریشه و root package همگام؛ dependencyها حفظ می‌شوند. |
| `README.md` | دستور نصب دقیق نسخهٔ جاری همگام می‌شود. |
| CLI، API و admin | محل‌هایی که `_version` را import می‌کنند نسخهٔ جدید را می‌گیرند؛ literal تازه اضافه نکنید. |
| GitHub release workflow | checker موجود اکنون drift تمام فایل‌های بالا را هم رد می‌کند؛ tag باید با نسخه مطابق باشد. |

فایل‌های همگام‌شونده را دستی نسخه‌گذاری نکنید. checker در CI انتشار اجرا می‌شود.
این ابزار فعلاً فقط نسخهٔ سه‌بخشی `X.Y.Z` می‌پذیرد؛ پشتیبانی PEP 440 prerelease و
تبدیل آن به SemVer/CMake باید جدا طراحی شود. در صورت نیاز به RC، صرفاً suffix
را دستی به یکی از فایل‌ها اضافه نکنید.

نسخهٔ schema، storage format، protocol، dependencyها و benchmarkهای تاریخی مستقل
هستند و نباید همراه شمارهٔ محصول کورکورانه تغییر کنند. release noteهای قبلی،
گزارش‌های اندازه‌گیری و source snapshotها سابقه‌اند. لینک release قبلی README
نیز سابقه است و ابزار آن را به فایل ناموجود نسخهٔ بعد تغییر نمی‌دهد.

## قبل از انتشار ۰.۴.۰

- [x] آزمون حافظهٔ ۱۰M با دو تکرار برای native نامزد ۰.۴.۰ قبول شد؛ شواهد و محدودیت‌ها در `releases/validation-v0.4.0.md` ثبت شده‌اند.
- [x] محدودهٔ نسخهٔ آزمایشی و محدودیت‌های document/graph/security مشخص شود؛ مرجع: `releases/v0.4.0.md`.
- [ ] تغییرات working tree بازبینی و commit نامزد انتشار مشخص شود.
- [x] فرمان تغییر نسخه به ۰.۴.۰ اجرا و checker پاس شد.
- [x] native و dashboard از سورس نامزد ۰.۴.۰ rebuild شدند؛ آزمون نصب نسخهٔ native را تأیید کرد.
- [x] wheel از staging تازه ساخته، با auditwheel repair و در محیط تمیز از خارج مخزن نصب شد.
- [x] `scripts/verify_installed_package.py --expected 0.4.0`، ۱۴۰ تست C++ و ۹۰ تست Python مرتبط پاس شدند.
- [x] release notes جدید با تغییرات، upgrade و محدودیت‌ها در `releases/v0.4.0.md` نوشته شد.
- [ ] پس از بازبینی، tag و release با شمارهٔ یکسان ساخته شوند؛ ابزار نسخه‌گذاری انتشار نمی‌کند.

در بررسی ۲۰۲۶-۱۰-۲۳، اجرای جدید ۱۰M با دو تکرار، صحت کامل، صفر افزایش
throttling/OOM و cleanup موفق تأیید شد. این پذیرش به ۱۰M محدود است؛
پایداری ۱۰۰M هنوز تأیید نشده است. بازبینی commit، CI و تصمیم انتشار باز هستند.

شواهد آماده‌سازی مستقل از آزمون بزرگ: [گزارش بررسی نامزد](releases/validation-v0.4.0.md).
