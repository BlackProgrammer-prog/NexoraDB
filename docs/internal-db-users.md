# NexoraDB Internal Database Users

این سند قرارداد نهایی بین بک‌اند Python/FastAPI و هسته C++ NexoraDB برای نگهداری کاربران داخلی دیتابیس است. هدف این است که اطلاعات کاربران دیتابیس مثل root/admin/application user در یک فضای داخلی ذخیره شود، اما برای کاربران عادی دیتابیس، queryهای عمومی و پنل ادمین قابل مشاهده یا دسترسی نباشد.

## هدف

NexoraDB باید یک collection داخلی و مخفی برای identity و دسترسی کاربران دیتابیس داشته باشد. این collection برای داده‌های application نیست و نباید مثل collectionهای معمولی رفتار شود.

این collection فقط توسط backend trusted، یعنی FastAPI admin/auth service، استفاده می‌شود. کاربر دیتابیس نباید بتواند آن را در `list_collections` ببیند، روی آن query بزند، حذف کند، export کند، در پنل ادمین مشاهده کند، یا با نام مستقیم به آن دسترسی بگیرد.

## نام Collection داخلی

نام قطعی:

```text
__nexora_internal_users
```

قانون قطعی برای کل سیستم:

هر collection که با prefix زیر شروع شود، system/internal محسوب می‌شود:

```text
__nexora_
```

این prefix باید reserved باشد. کاربر نباید بتواند collection معمولی با این prefix بسازد.

## اصلاح مهم در طرح اولیه

پسورد خام نباید در C++ یا RocksDB ذخیره شود.

بک‌اند FastAPI باید پسورد را دریافت کند، validate کند، با الگوریتم امن hash کند و فقط hash نهایی را به NexoraDB بدهد. فرمت ذخیره:

```json
{
  "password_hash": "$argon2id$v=19$m=65536,t=3,p=4$..."
}
```

الگوریتم محصول: `argon2id`. هسته C++ فقط hashهایی را قبول می‌کند که با یکی از prefixهای زیر شروع شوند:

```text
$argon2id$
$2a$
$2b$
$2y$
```

سه prefix آخر برای `bcrypt` هستند و فقط برای سازگاری پذیرفته می‌شوند.

هیچ API در C++ متدی مثل `check_password(raw_password)` ندارد. مسئولیت hash و verify با FastAPI است.

## مدل داده

هر user داخلی یک JSON document از سمت FastAPI دریافت می‌شود و در C++ مثل BSON/JSON فعلی ذخیره می‌شود.

نمونه root/admin اولیه:

```json
{
  "_id": "usr_root",
  "username": "root",
  "email": "admin@example.com",
  "password_hash": "$argon2id$v=19$m=65536,t=3,p=4$...",
  "role": "admin",
  "first_name": "Database",
  "last_name": "Administrator",
  "status": "active",
  "created_at": 1782560000000,
  "updated_at": 1782560000000,
  "last_login_at": null
}
```

نمونه application user:

```json
{
  "_id": "usr_app_01",
  "username": "analytics_app",
  "email": null,
  "password_hash": "$argon2id$v=19$m=65536,t=3,p=4$...",
  "role": "application",
  "first_name": null,
  "last_name": null,
  "status": "active",
  "created_at": 1782560000000,
  "updated_at": 1782560000000,
  "last_login_at": null
}
```

## فیلدها

- `_id`: شناسه داخلی user. backend آن را تولید می‌کند. برای root مقدار ثابت `usr_root` است.
- `username`: اجباری و unique. برای ادمین اولیه باید `root` باشد.
- `email`: برای admin اجباری؛ برای application user می‌تواند `null` باشد.
- `password_hash`: اجباری. هرگز password خام ذخیره نشود.
- `role`: یکی از `admin` یا `application`.
- `first_name`: برای admin اجباری است؛ برای application user اختیاری است.
- `last_name`: برای admin اجباری است؛ برای application user اختیاری است.
- `status`: یکی از `active`, `disabled`, `deleted`.
- `created_at`: unix timestamp بر حسب millisecond.
- `updated_at`: unix timestamp بر حسب millisecond.
- `last_login_at`: unix timestamp بر حسب millisecond یا `null`.

## نقش‌ها

نسخه فعلی فقط دو نقش دارد:

```text
admin
application
```

تعریف نهایی:

- `admin`: اجازه مدیریت دیتابیس، ساخت کاربر، مشاهده وضعیت دیتابیس و تنظیمات.
- `application`: کاربر مخصوص اتصال اپلیکیشن‌ها به دیتابیس. دسترسی دقیق collectionها و عملیات‌ها بعداً می‌تواند با permissionهای جدا اضافه شود.

برای آینده، schema اجازه اضافه شدن این فیلد را خواهد داشت:

```json
"permissions" : [
  {"collection": "users", "actions": ["read", "write"]},
  {"collection": "orders", "actions": ["read"]}
]
```

اما در نسخه اول لازم نیست.

## Bootstrap نصب اولیه

وقتی دیتابیس برای اولین بار نصب یا initialize می‌شود:

1. FastAPI بررسی می‌کند آیا root user وجود دارد یا نه.
2. اگر وجود ندارد، اطلاعات admin اولیه را از setup flow می‌گیرد.
3. پسورد خام را hash می‌کند.
4. متد داخلی C++ برای ساخت internal user را صدا می‌زند.
5. اگر root وجود داشت، setup نباید دوباره root جدید بسازد.

قانون مهم:

فقط یک user با `username = root` مجاز است.

## API نهایی C++ برای FastAPI

این متدها باید internal باشند و با CRUD عمومی collectionها فرق داشته باشند.

نام‌های قطعی در pybind11:

```python
engine.create_internal_user(user_json: str) -> DBResult
engine.get_internal_user(username: str) -> DBResult
engine.update_internal_user(username: str, user_json: str) -> DBResult
engine.delete_internal_user(username: str) -> DBResult
```

رفتار مورد انتظار:

- `create_internal_user`: JSON کامل user را از FastAPI می‌گیرد و در `__nexora_internal_users` ذخیره می‌کند.
- `get_internal_user`: فقط برای backend auth استفاده می‌شود. کاربر عادی نباید به آن دسترسی داشته باشد.
- `update_internal_user`: جایگزینی کامل سند کاربر است. مقدار `username` داخل JSON باید با پارامتر `username` یکی باشد.
- `delete_internal_user`: hard delete انجام نمی‌دهد؛ مقدار `status` را به `deleted` تغییر می‌دهد و `updated_at` را به timestamp فعلی می‌برد.

نکته ذخیره‌سازی:

در RocksDB، کلید سند internal user برابر `username` است. فیلد `_id` داخل JSON می‌تواند مثل نمونه‌ها مقدار جداگانه‌ای داشته باشد، اما lookup اصلی با `username` انجام می‌شود.

برای login معمولاً FastAPI به این نیاز دارد:

```python
engine.get_internal_user("root")
```

بعد FastAPI مقدار `password_hash` را می‌گیرد و password خام کاربر را سمت Python verify می‌کند.

## قوانین امنیتی سمت C++

هسته C++ باید این invariantها را نگه دارد:

- هیچ collection عمومی با prefix `__nexora_` ساخته نمی‌شود.
- `list_collections()` internal collectionها را برنمی‌گرداند.
- `collection_exists("__nexora_internal_users")` برای API عمومی مقدار `false` برمی‌گرداند.
- APIهای عمومی CRUD و مدیریت collection/index/fk روی collectionهای `__nexora_*` خطای `reserved collection name` برمی‌گردانند.
- فقط متدهای internal user اجازه دسترسی به این collection را داشته باشند.
- پنل ادمین نباید این collection را از API عمومی دریافت کند.
- export/backup عمومی اگر بعداً اضافه شد باید system collections را جداگانه و فقط با سطح دسترسی admin مدیریت کند.

## قرارداد خطاها

خروجی مثل بقیه متدهای `DocEngine` باشد:

```json
{
  "success": false,
  "data": "",
  "error_msg": "internal user not found"
}
```

پیام‌های قطعی:

- `internal user already exists`
- `internal user not found`
- `username is required`
- `password_hash is required`
- `invalid role`
- `invalid status`
- `password_hash must be argon2id or bcrypt`
- `admin requires email, first_name and last_name`
- `username cannot be changed`
- `root user must be admin`
- `cannot delete root user`
- `reserved collection name`

## اعتبارسنجی نهایی

FastAPI باید قبل از ارسال JSON به C++ این موارد را validate کند:

- `username` خالی نباشد.
- برای admin، `email`, `first_name`, `last_name` خالی نباشند.
- `role` فقط `admin` یا `application` باشد.
- `password_hash` وجود داشته باشد و با prefix معتبر `argon2id` یا `bcrypt` شروع شود.
- `status` معتبر باشد.
- اگر `username` برابر `root` است، `role` باید `admin` باشد.
- در update، مقدار `username` داخل JSON نباید با پارامتر مسیر متفاوت باشد.

C++ هم باید حداقل validation ضروری را دوباره انجام دهد، چون invariantهای دیتابیس نباید فقط به FastAPI وابسته باشند.

## مواردی که نباید انجام شود

- پسورد خام ذخیره نشود.
- collection داخلی در `list_collections()` نمایش داده نشود.
- FastAPI از CRUD عمومی برای این collection استفاده نکند.
- UI پنل ادمین نام `__nexora_internal_users` را نمایش ندهد.
- حذف root user مجاز نباشد. اگر لازم شد فقط password یا profile آن تغییر کند.

## وضعیت پیاده‌سازی در C++

1. در `DocEngine` این helper خصوصی اضافه شده است:

```cpp
static bool IsReservedCollectionName(const std::string& name);
```

2. در CRUD عمومی، اگر collection با `__nexora_` شروع شود، خطای `reserved collection name` برمی‌گردد.

3. در constructor داخلی، collection `__nexora_internal_users` به صورت system collection آماده می‌شود.

4. چهار متد internal user در C++ اضافه و در pybind11 expose شده‌اند.

5. FastAPI فقط همین چهار متد را برای مدیریت کاربران دیتابیس صدا بزند.

6. بعداً اگر permission system اضافه شد، application userها می‌توانند permissionهای collection-level بگیرند.

