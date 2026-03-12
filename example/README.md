# SQLite Auth Example

Bu klasor, ViperLang ile yazilmis kapsamli bir auth ornegi icerir:

- `auth_sqlite.vp`

Ozellikler:

- SQLite3 tabanli `users`, `auth_sessions`, `audit_logs` tablolari
- Register/Login
- JWT imzali access + refresh token akisi
- Session rotate (`/auth/refresh`)
- Session dogrulama (`/auth/me`)
- Logout ve Logout-All
- Sifre degistirme (tum sessionlari iptal eder)
- Basit login rate-limit + account lock
- Audit log

## Calistirma

1. Runtime'i derle:

```bash
make
```

2. Servisi baslat:

```bash
./viper example/auth_sqlite.vp
```

Servis varsayilan olarak `8090` portunu dinler.

## Ornek Istekler

Kayit:

```bash
curl -sS -X POST http://127.0.0.1:8090/auth/register \
  -H 'Content-Type: application/json' \
  -d '{"email":"admin@example.com","name":"Admin","password":"StrongPass123"}'
```

Login:

```bash
curl -sS -X POST http://127.0.0.1:8090/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"email":"admin@example.com","password":"StrongPass123"}'
```

Me:

```bash
curl -sS http://127.0.0.1:8090/auth/me \
  -H "Authorization: Bearer <ACCESS_TOKEN>"
```

Refresh:

```bash
curl -sS -X POST http://127.0.0.1:8090/auth/refresh \
  -H 'Content-Type: application/json' \
  -d '{"refresh_token":"<REFRESH_TOKEN>"}'
```

Logout:

```bash
curl -sS -X POST http://127.0.0.1:8090/auth/logout \
  -H "Authorization: Bearer <ACCESS_TOKEN>"
```

## Notlar

- Token payload'i debug amacli okunabilir bir formattadir.
- `AUTH_SECRET` ve `PASS_PEPPER` degerlerini prod ortamina gore degistir.
- Bu ornek egitim/prototip amacli; production'a cikmadan once ek sertlestirme gerekir.
