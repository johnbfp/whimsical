@echo off
REM ==========================================================
REM  Whimsical Agent Runtime - Local Development Launcher
REM  Starts backend (Python) + executor (Rust) + frontend (Vue)
REM ==========================================================

echo ============================================
echo   Whimsical Agent Runtime - Dev Launcher
echo ============================================

REM --- 1. Start Rust Executor ---
echo.
echo [1/3] Starting Rust Executor (port 8088)...
cd executor-rs
start "Executor" cmd /k "cargo run"
cd ..

REM --- 2. Start Python Backend ---
echo [2/3] Starting Python Backend (port 8000)...
cd backend
start "Backend" cmd /k "pip install -e .[test] 2>nul & uvicorn app.main:app --host 0.0.0.0 --port 8000 --reload"
cd ..

REM --- 3. Start Vue Frontend ---
echo [3/3] Starting Vue Frontend (port 5173)...
cd frontend
start "Frontend" cmd /k "npm install & npm run dev"
cd ..

echo.
echo ============================================
echo   All services starting...
echo.
echo   Frontend:  http://localhost:5173
echo   Backend:   http://localhost:8000
echo   Executor:  http://localhost:8088
echo   API Docs:  http://localhost:8000/docs
echo ============================================
echo.
echo Press any key to open the browser...
pause >nul
start http://localhost:5173
