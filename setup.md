## Setup Instructions

### 1. Clone repository
git clone https://github.com/sebarreto/voice-ai-agent.git
cd voice-ai-agent

### 2. Create virtual environment
python -m venv .venv
source .venv/bin/activate

### 3. Install dependencies
pip install -r backend/requirements.txt

### 4. Configure environment variables
cp backend/.env.example backend/.env

### 4. Configure environment variables
cp backend/.env.data backend/.env
Fill in Azure Keys!!!

### 5. Populate vector database
python vector_db/populate_vector.py
Done!!!

### 6. Run backend
python backend/backend.py
Check IP

### 7. Open frontend
Open frontend/tekavoz_ia.html in a browser
