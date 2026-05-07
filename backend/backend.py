from urllib import response
from azure.search.documents import SearchClient
from azure.core.credentials import AzureKeyCredential
from flask import Flask, request, jsonify
import openai
from openai import AzureOpenAI
from flask_cors import CORS
from azure.search.documents.models import VectorizedQuery
import os
import requests
from dotenv import load_dotenv

load_dotenv('.env.data')

app = Flask(__name__)
CORS(app)

history = []
SEARCH_ENDPOINT = os.getenv("AZURE_SEARCH_ENDPOINT")
SEARCH_KEY = os.getenv("AZURE_SEARCH_KEY")
INDEX_NAME = os.getenv("AZURE_SEARCH_INDEX")

# Config Azure OpenAI
client = AzureOpenAI(
    api_key=os.getenv("AZURE_OPENAI_KEY"),
    api_version=os.getenv("AZURE_OPENAI_API_VERSION"),
    azure_endpoint=os.getenv("AZURE_OPENAI_ENDPOINT")
)

search_client = SearchClient(
    endpoint=SEARCH_ENDPOINT,
    index_name=INDEX_NAME,
    credential=AzureKeyCredential(SEARCH_KEY)
)

def get_embedding(text: str):
    response = client.embeddings.create(
        model="text-embedding-3-large",  
        input=text
    )
    return response.data[0].embedding

def search_docs(query: str, k: int = 3):
    embedding = get_embedding(query)
 
    vector_query = VectorizedQuery(
        vector=embedding,
        k_nearest_neighbors=k,
        fields="embedding"
    )
 
    results = search_client.search(
        search_text=None,
        vector_queries=[vector_query],
        select=["content"]
    )
 
    docs = []
    for r in results:
        docs.append(r["content"])
 
    return docs

history = []
@app.route("/api/chat", methods=["POST"])
def chat():
    global history
   #user_question = request.json["question"]
    user_question = request.json.get("question", "").strip()
    print(f"Received question: {user_question}")
    if len(user_question) < 4:
        return jsonify({"answer": ""})

     # 1. Look for context in Azure Search
    docs = search_docs(user_question)
    context = "\n".join(docs)
     # 2. Save conversation history
    if history and history[-1]["role"] == "user":
        if history[-1]["content"].lower() == user_question.lower():
            return jsonify({"answer": ""})
    history.append({"role": "user", "content": user_question})

    safe_history = []

    for m in history:
        if m.get("content"):
            safe_history.append({
                "role": m["role"],
                "content": str(m["content"])
            })

     # 3. Call GPT
    response = client.chat.completions.create(
        model="gpt-4o-mini",
        messages=[
            {
                "role": "system",
                "content": """You are a smart cooking assistant for Teka appliances.
                Rules:
                - Answer in English
                - Use manual data when relevant
                - If not available, use cooking knowledge
                - Always include appliance usage (temperature, power)
                - Be step-by-step but concise, short sentences, max 5 steps.
                - Don't use caracters like "#", "-", "1.", "2.", just new lines for steps.
                - Short answers, max 100 tokens.
                """
            },
            *safe_history,  
            {
                "role": "user",
                "content": f"Context:\n{context}\n\nQuestion:\n{user_question}"
            }
        ],
        temperature=0.3,
        max_tokens=100
    )
    answer = response.choices[0].message.content
    history.append({"role": "assistant", "content": answer})
    if not answer:
        answer = "Sorry, I couldn't generate a response."
    return jsonify({"answer": answer})

@app.route("/api/speech-token", methods=["GET"])
def get_speech_token():
    speech_key = os.getenv("AZURE_SPEECH_KEY")
    region = os.getenv("AZURE_SPEECH_REGION")

    url = f"https://{region}.api.cognitive.microsoft.com/sts/v1.0/issueToken"
    headers = {
        "Ocp-Apim-Subscription-Key": speech_key
    }

    token = requests.post(url, headers=headers)
    return jsonify({
        "token": token.text,
        "region": region
    })

@app.route('/auth-response')
def auth_response():
    print(request.args) 
    print("auth-response endpoint hit")
    return 'Redirección exitosa'

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)  