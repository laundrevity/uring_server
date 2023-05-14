import os
import sys
import subprocess
import datetime
import traceback
import openai
openai.api_key = os.getenv('OPENAI_API_KEY')

def get_file_content(file_path: str):
    with open(file_path, 'r') as f:
        return f.read()

def write_source_code():
    current_datetime = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    uname_output = subprocess.run(['uname', '-a'], capture_output=True, text=True).stdout.strip()

    state_content = f"{uname_output}\n"
    os.chdir("/home/conor/sandbox/uring")
    print(os.getcwd())

    for file_name in os.listdir('.'):
        if file_name.endswith('.py') or file_name.endswith('CMakeLists.txt') or file_name.endswith('cpp'):
            state_content += f"--- {file_name} ---\n"
            state_content += get_file_content(file_name)
            state_content += "\n\n"

    with open('state.txt', 'w') as f:
        f.write(state_content)


def write_state_file(build_successful: bool, server_output_msg: str, client_output_msg: str, command: str):
    write_source_code()
    state_content = ""
    state_content += f"Command used: {command}\n\n"
    state_content += f"build datetime: {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')}\n\n"
    state_content += f"Server output messages:\n{server_output_msg}\n\n"
    state_content += f"Client output messages:\n{client_output_msg}\n\n"

    os.chdir("/home/conor/sandbox/uring")
    with open(f"state.txt", "a") as f:
        f.write(state_content)

def build():
    try:
        if not os.path.exists('build'):
            command = ["mkdir", "build"]
            subprocess.run(command, check=True)

        os.chdir("build")

        command = ["cmake", "..", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        command = ["cmake", "--build", "."]  # Corrected the build command
        result2 = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        server_output_msg = f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        client_output_msg = f"stdout:\n{result2.stdout}\nstderr:\n{result2.stderr}"

        build_successful = result.returncode == 0 and result2.returncode == 0

        write_state_file(build_successful, server_output_msg, client_output_msg, command)

    except Exception as e:
        print(f"Error: {e}")
        formatted_traceback = traceback.format_exc()
        write_state_file(False, f"Traceback:\n{formatted_traceback}", "", command)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: python main.py <server-binary> <client-binary> [--send]")
        sys.exit(1)

    if len(sys.argv) >= 4:
        send_gpt4 = True
    else:
        send_gpt4 = False

    build()

    if send_gpt4:
        os.chdir("/home/conor/sandbox/uring")
        prompt = open('prompt.txt').read()
        state = open('state.txt').read()
        messages = [
                {
                    'role': 'system',
                    'content': state
                },
                {
                    'role': 'user',
                    'content': prompt
                }
            ]

        print(f'sending with prompt: {prompt}...')
        response = openai.ChatCompletion.create(
            model='gpt-4',
            messages=messages,
            stream=True
        )

        answer = ''
        timestamp = datetime.datetime.now().strftime('%H_%M_%S')
        response_filename = f"response_{timestamp}.md"

        with open(response_filename, "w") as response_file:
            for chunk in response:
                try:
                    s = chunk['choices'][0]['delta']['content']

                    answer += s
                    print(s, end='')
                    response_file.write(s)
                    response_file.flush()  # ensure content is written to file immediately
                except:
                    pass