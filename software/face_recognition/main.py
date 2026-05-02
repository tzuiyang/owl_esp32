import face_recognition
import os

KNOWN_FACES_DIR = "known_faces"
TEST_IMAGE = "test.jpg"

known_encodings = []
known_names = []

# Load known faces
for name in os.listdir(KNOWN_FACES_DIR):
    for filename in os.listdir(f"{KNOWN_FACES_DIR}/{name}"):
        path = f"{KNOWN_FACES_DIR}/{name}/{filename}"
        image = face_recognition.load_image_file(path)
        encodings = face_recognition.face_encodings(image)

        if len(encodings) > 0:
            known_encodings.append(encodings[0])
            known_names.append(name)

print("Loaded known faces:", known_names)

# Load test image
test_image = face_recognition.load_image_file(TEST_IMAGE)
test_encodings = face_recognition.face_encodings(test_image)

for face_encoding in test_encodings:
    matches = face_recognition.compare_faces(known_encodings, face_encoding)

    name = "Unknown"

    if True in matches:
        first_match_index = matches.index(True)
        name = known_names[first_match_index]

    print(f"Detected: {name}")
