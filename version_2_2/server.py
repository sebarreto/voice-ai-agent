import asyncio
import os
from fastapi import FastAPI, WebSocket
import azure.cognitiveservices.speech as speechsdk
from dotenv import load_dotenv
from openai import AzureOpenAI
from azure.search.documents import SearchClient
from azure.search.documents.models import VectorizedQuery
from azure.core.credentials import AzureKeyCredential
from contextlib import asynccontextmanager
import azure.cognitiveservices.speech as speechsdk
import asyncio

# ================= CONFIG =================
load_dotenv('.env.data')

SPEECH_KEY = os.getenv("AZURE_SPEECH_KEY")
SPEECH_REGION = os.getenv("AZURE_SPEECH_REGION")
AZURE_OPENAI_KEY = os.getenv("AZURE_OPENAI_KEY")
AZURE_OPENAI_ENDPOINT = os.getenv("AZURE_OPENAI_ENDPOINT")
AZURE_OPENAI_API_VERSION = os.getenv("AZURE_OPENAI_API_VERSION")
SEARCH_ENDPOINT = os.getenv("AZURE_SEARCH_ENDPOINT")
SEARCH_KEY = os.getenv("AZURE_SEARCH_KEY")
INDEX_NAME = os.getenv("AZURE_SEARCH_INDEX")

# ================= CLIENTS =================
client = AzureOpenAI(
    api_key=AZURE_OPENAI_KEY,
    api_version=AZURE_OPENAI_API_VERSION,
    azure_endpoint=AZURE_OPENAI_ENDPOINT
)

search_client = SearchClient(
    endpoint=SEARCH_ENDPOINT,
    index_name=INDEX_NAME,
    credential=AzureKeyCredential(SEARCH_KEY)
)

# ================= LIFESPAN =================
@asynccontextmanager
async def lifespan(app: FastAPI):
    print("🔥 Warming up...")
    client.chat.completions.create(
        model="gpt-4o-mini",
        messages=[{"role": "user", "content": "hi"}],
        max_tokens=5
    )
    speech_config = speechsdk.SpeechConfig(subscription=SPEECH_KEY, region=SPEECH_REGION)
    synthesizer = speechsdk.SpeechSynthesizer(speech_config=speech_config, audio_config=None)
    synthesizer.speak_text_async("ok").get()
    print("✅ Warmup done")
    yield
    print("Shutting down app...")

app = FastAPI(lifespan=lifespan)

# ================= HELPERS =================
def normalize_text(text: str) -> str:
    text = text.lower()
    for k, v in {"ovum": "oven", 
                 "tekka": "teka", 
                 "dekha": "teka", 
                 "cup tok": "cooktop", 
                 "cup top": "cooktop",  
                 "pen": "pan", 
                 "pant": "pan", 
                 "tika": "teka"
                 }.items():
        text = text.replace(k, v)
    return text

def is_valid_utterance(text: str) -> bool:
    clean = text.strip(" .,?!*-_")
    if len(clean) < 3:
        return False
    if all(c in "*-_. " for c in clean):
        return False
    return True

def fast_intent(text: str):
    text = text.lower()
    if "turn on" in text:
        return "Turning on the cooktop"
    if "turn off" in text:
        return "Turning off the cooktop"
    if "power" in text:
        return "Adjusting power level"
    if "i feel hot" in text:
        return {
            "intent": "SET_AC",
            "temperature": 22
        }
    if "i feel cold" in text:
        return {
            "intent": "SET_AC",
            "temperature": 25
        }
    if "turn on air conditioner" in text:
        return {
            "intent": "SET_AC",
            "temperature": 22
        }
    if "turn off air conditioner" in text:
        return {
            "intent": "AC_OFF"
        }
    return None

def needs_rag(text: str) -> bool:
    keywords = ["error", "code", "fault", "manual", "instruction", "clean", "maintenance"]
    return any(k in text.lower() for k in keywords)

def get_embedding(text: str):
    response = client.embeddings.create(model="text-embedding-3-large", input=text)
    return response.data[0].embedding

def search_docs(query: str, k: int = 3):
    print("🔍 Searching RAG")
    embedding = get_embedding(query)
    print("🔍 Embedding done")
    results = search_client.search(
        search_text=None,
        vector_queries=[VectorizedQuery(vector=embedding, k_nearest_neighbors=k, fields="embedding")],
        select=["content"]
    )
    print("🔍 Search done")
    return "\n".join(r["content"] for r in results)

async def execute_action(intent):
    if intent["intent"] == "SET_AC":
        temp = intent["temperature"]
        print(f"🏠 AC -> {temp}°C")
        # future MQTT call
        # mqtt.publish(...)
        return (
            "I'll make the room more comfortable. "
            f"Air conditioner set to {temp} degrees."
        )
    if intent["intent"] == "AC_OFF":
        print("🏠 AC OFF")
        return "Air conditioner switched off."
    return "Done."
# ================= LLM =================
MAX_HISTORY = 4

async def process_with_rag(text: str, conversation: list) -> str:
    intent = fast_intent(text)
    if intent:
        return await execute_action(intent)

    if needs_rag(text):
        context = await asyncio.to_thread(search_docs, text)
        prompt = f"""You are a house assistant.
Use FIRST the following manual context to answer.
If answer is not in the manual, use your general knowledge.
Manual:
{context}
Question:
{text}"""
    else:
        prompt = text

    messages = [{
        "role": "system",
        "content": (
            "You are a smart house assistant. "
            "Answer in English. Use manual data when relevant, otherwise use general knowledge. "
            "Always include appliance usage (temperature, power). "
            "Be step-by-step but concise, short sentences, max 4 steps. "
            "Do not use characters like #, -, 1., 2. — just new lines for steps. "
            "Answers as short as possible, ideally under 20 words."
        )
    }]

    for m in conversation:
        messages.append(m)
    messages.append({"role": "user", "content": prompt})

    response = await asyncio.to_thread(
        client.chat.completions.create,
        model="gpt-4o-mini",
        messages=messages,
        max_tokens=100,
        temperature=0.3
    )

    answer = response.choices[0].message.content

    conversation.append({"role": "user", "content": text})
    conversation.append({"role": "assistant", "content": answer})
    if len(conversation) > MAX_HISTORY * 2:
        del conversation[:-MAX_HISTORY * 2]

    return answer

# ================= TTS =================

async def tts_stream(text: str):
    speech_config = speechsdk.SpeechConfig(
        subscription=SPEECH_KEY,
        region=SPEECH_REGION
    )

    speech_config.set_speech_synthesis_output_format(
        speechsdk.SpeechSynthesisOutputFormat.Raw16Khz16BitMonoPcm
    )

    synthesizer = speechsdk.SpeechSynthesizer(
        speech_config=speech_config,
        audio_config=None
    )

    # Start synthesis
    result = await asyncio.to_thread(
        synthesizer.speak_text_async(text).get
    )

    if result.reason != speechsdk.ResultReason.SynthesizingAudioCompleted:
        print("❌ TTS failed")
        return

    # Create stream reader
    stream = speechsdk.AudioDataStream(result)

    buffer = bytes(1024)  # IMPORTANT: immutable bytes, NOT bytearray

    while True:
        size = stream.read_data(buffer)

        if size == 0:
            break

        yield buffer[:size]

        await asyncio.sleep(0)

# ================= WEBSOCKET =================
@app.websocket("/ws/audio")
async def websocket_audio(ws: WebSocket):
    await ws.accept()
    print("✅ WebSocket accepted")

    is_activated = False
    conversation = []

    speech_config = speechsdk.SpeechConfig(subscription=SPEECH_KEY, region=SPEECH_REGION)
    speech_config.speech_recognition_language = "en-US"

    audio_format = speechsdk.audio.AudioStreamFormat(
        samples_per_second=16000,
        bits_per_sample=16,
        channels=1
    )
    stream = speechsdk.audio.PushAudioInputStream(
        stream_format=audio_format
    )
    audio_config = speechsdk.audio.AudioConfig(stream=stream)
    recognizer = speechsdk.SpeechRecognizer(speech_config=speech_config, audio_config=audio_config)
    print("✅ Recognizer created")

    text_queue = asyncio.Queue()
    loop = asyncio.get_event_loop()   # ← captured once in async context

    WAKE_WORD = "assistant"
    SESSION_TIMEOUT = 45
    FILLER_WORDS = {"hello", "hi", "hey", "ok", "okay", "yes", "no"}

    last_activity = loop.time()

    stop_requested = False

    STOP_WORDS = {"stop", "cancel", "enough", "shut up", "quiet", "silence"}

    ws_lock = asyncio.Lock()

    def on_recognized(evt):
        nonlocal is_activated, last_activity, stop_requested
        try:
            last_activity = loop.time()
            text = evt.result.text.strip()
            print(f"🎯 Heard: '{text}'")

            if not text or not is_valid_utterance(text):
                return

            # Stop command works whether activated or not
            text_clean = text.lower().strip(" .,?!")
            if any(word in text_clean for word in STOP_WORDS):
                print("🛑 Stop requested")
                stop_requested = True
                return

            if not is_activated:
                if WAKE_WORD in text.lower():
                    is_activated = True
                    stop_requested = False   # ← clear on wake
                    print("✅ Wake word detected!")
                    clean = text.lower()
                    for phrase in ["hello assistant", "hey assistant", "assistant"]:
                        clean = clean.replace(phrase, "")
                    clean = clean.strip(" ,.?!")
                    loop.call_soon_threadsafe(text_queue.put_nowait, clean if clean else "__wake__")
            else:
                if text.lower().strip(" .,?!") in FILLER_WORDS:
                    print(f"⏭️ Skipping filler: '{text}'")
                    return
                print(f"💬 Queuing: '{text}'")
                loop.call_soon_threadsafe(text_queue.put_nowait, text)

        except Exception as e:
            import traceback
            print(f"💥 CRASH in on_recognized: {e}")
            traceback.print_exc()

    def on_canceled(evt):
        print(f"❌ CANCELED: reason={evt.reason} details={evt.cancellation_details.error_details}")

    def on_session_started(evt):
        print("🟢 Recognition session started")

    def on_session_stopped(evt):
        print("🔴 Recognition session stopped")

    recognizer.recognized.connect(on_recognized)
    recognizer.canceled.connect(on_canceled)
    recognizer.session_started.connect(on_session_started)
    recognizer.session_stopped.connect(on_session_stopped)
    recognizer.start_continuous_recognition()
    print("✅ Recognition started")

    async def receive_audio():
        count = 0
        try:
            while True:
                data = await ws.receive_bytes()
                stream.write(data)
                if count % 50 == 0:
                    print(
                        f"chunk={count} bytes={len(data)}"
                    )
                count += 1
                if count % 50 == 0:
                    print(f"📦 Chunks received: {count}")
        except Exception as e:
            print(f"⚠️ receive_audio closed: {e}")
            stream.close()
            recognizer.stop_continuous_recognition()
    
    async def process():
        nonlocal last_activity, stop_requested

        while True:
            try:
                text = normalize_text(await text_queue.get())
                print(f"📨 Processing: {text}")

                stop_requested = False

                if text == "__wake__":
                    response_text = "Hello! How can I help you?"
                else:
                    try:
                        response_text = await asyncio.wait_for(
                            process_with_rag(text, conversation),
                            timeout=20
                        )
                    except asyncio.TimeoutError:
                        print("❌ LLM timeout")
                        response_text = "Sorry, I took too long to respond."


                print(f"🤖 Assistant: {response_text}")

                last_activity = loop.time()

                started = False

                async for chunk in tts_stream(response_text):

                    if not started:
                        print("🔊 TTS streaming started")
                        started = True

                    if stop_requested:
                        print("🛑 TTS aborted")
                        break

                    await ws.send_bytes(chunk)
                
                await ws.send_text("END_AUDIO")

                # Notify ESP32 playback finished
                async with ws_lock:
                    await ws.send_text("END_AUDIO")

            except Exception as e:
                import traceback
                print(f"⚠️ process error: {e}")
                traceback.print_exc()
                try:
                    async for chunk in tts_stream("Sorry, I had a problem with that."):
                        await ws.send_bytes(chunk)
                except:
                    pass

    async def session_watcher():
        nonlocal is_activated
        while True:
            await asyncio.sleep(1)
            if is_activated and loop.time() - last_activity > SESSION_TIMEOUT:
                is_activated = False
                print("💤 Session timeout — back to sleep.")

    await asyncio.gather(receive_audio(), process(), session_watcher())
