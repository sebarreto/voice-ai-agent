import uuid
import fitz  # PyMuPDF
from azure.search.documents import SearchClient
from azure.core.credentials import AzureKeyCredential
from openai import AzureOpenAI
from azure.ai.formrecognizer import DocumentAnalysisClient
from azure.core.credentials import AzureKeyCredential
import os
from dotenv import load_dotenv

load_dotenv('.env.data')

# CONFIG
SEARCH_ENDPOINT = os.getenv("AZURE_SEARCH_ENDPOINT")
SEARCH_KEY = os.getenv("AZURE_SEARCH_KEY")
INDEX_NAME = os.getenv("AZURE_SEARCH_INDEX")
 
client_ai = AzureOpenAI(
    api_key=os.getenv("AZURE_OPENAI_KEY"),
    azure_endpoint=os.getenv("AZURE_OPENAI_ENDPOINT"),
    api_version="2024-12-01-preview"
)
 
search_client = SearchClient(
    endpoint=SEARCH_ENDPOINT,
    index_name=INDEX_NAME,
    credential=AzureKeyCredential(SEARCH_KEY)
)

endpoint = os.getenv("AZURE_OCR_ENDPOINT")
key = os.getenv("AZURE_OCR_KEY")

client_ocr = DocumentAnalysisClient(endpoint, AzureKeyCredential(key))

def extract_text_with_ocr(file_path):
    with open(file_path, "rb") as f:
        poller = client_ocr.begin_analyze_document(
            "prebuilt-read",  # 👈 OCR model
            document=f
        )
        result = poller.result()

    text = ""
    for page in result.pages:
        for line in page.lines:
            text += line.content + "\n"

    return text

def get_embedding(text):
    return client_ai.embeddings.create(
        model="text-embedding-3-large",
        input=text
    ).data[0].embedding
 
#def chunk_text(text, size=500, overlap=50):
#    chunks = []
#    start = 0
#    while start < len(text):
#        chunks.append(text[start:start+size])
#        start += size - overlap
#    return chunks

def chunk_text(text, max_size=800):
    sentences = text.split(". ")
    chunks = []
    current = ""

    for s in sentences:
        if len(current) + len(s) < max_size:
            current += s + ". "
        else:
            chunks.append(current.strip())
            current = s + ". "

    if current:
        chunks.append(current.strip())

    return chunks

def process_pdf(file_path):
    full_text = extract_text_with_ocr(file_path)
    chunks = chunk_text(full_text)
 
    docs = []
    for chunk in chunks:
        docs.append({
            "id": str(uuid.uuid4()),
            "content": chunk,
            "source": file_path,
            "embedding": get_embedding(chunk)
        })
 
    result = search_client.upload_documents(docs)

    for r in result:
        print(r.succeeded, r.key)
 
# Execute the function to process and upload the PDF content to Azure Cognitive Search
process_pdf("hob_manual_IZF_68770_5.pdf")