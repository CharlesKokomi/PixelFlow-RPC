const operations = [
  {
    type: 1,
    code: "01",
    label: "灰度化",
    description: "单通道灰度转换"
  },
  {
    type: 2,
    code: "02",
    label: "Canny",
    description: "边缘结构检测"
  },
  {
    type: 3,
    code: "03",
    label: "高斯模糊",
    description: "平滑滤波处理"
  },
  {
    type: 4,
    code: "04",
    label: "反色",
    description: "逐像素颜色反转"
  },
  {
    type: 5,
    code: "05",
    label: "阈值分割",
    description: "二值化前景提取"
  },
  {
    type: 6,
    code: "06",
    label: "卡通化",
    description: "生成卡通风格效果"
  }
];

const imageInput = document.querySelector("#image-input");
const fileName = document.querySelector("#file-name");
const fileType = document.querySelector("#file-type");
const fileSize = document.querySelector("#file-size");
const operationButtons = document.querySelector("#operation-buttons");
const clearButton = document.querySelector("#clear-button");
const originalImage = document.querySelector("#original-image");
const resultImage = document.querySelector("#result-image");
const originalFrame = originalImage.closest(".image-frame");
const resultFrame = resultImage.closest(".image-frame");
const statusBar = document.querySelector("#status-bar");
const statusMessage = document.querySelector("#status-message");
const activeOperation = document.querySelector("#active-operation");
const activeOperationDetail = document.querySelector("#active-operation-detail");

let selectedFile = null;
let originalUrl = "";
let resultUrl = "";
let isProcessing = false;

function setStatus(message, state = "") {
  statusMessage.textContent = message;
  statusBar.className = "status-bar";

  if (state) {
    statusBar.classList.add(`is-${state}`);
  }
}

function revokeUrl(url) {
  if (url) {
    URL.revokeObjectURL(url);
  }
}

function formatFileSize(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) {
    return "--";
  }

  const units = ["B", "KB", "MB", "GB"];
  let value = bytes;
  let unitIndex = 0;

  while (value >= 1024 && unitIndex < units.length - 1) {
    value /= 1024;
    unitIndex += 1;
  }

  return `${value.toFixed(value >= 10 || unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
}

function formatFileType(file) {
  if (!file || !file.type) {
    return "--";
  }

  return file.type.replace("image/", "").toUpperCase();
}

function setButtonsDisabled(disabled) {
  document.querySelectorAll(".operation-button").forEach((button) => {
    button.disabled = disabled;
  });
  clearButton.disabled = disabled;
}

function markActiveButton(type) {
  document.querySelectorAll(".operation-button").forEach((button) => {
    button.setAttribute("aria-pressed", String(button.dataset.type === String(type)));
  });
}

function setActiveOperation(operation) {
  if (!operation) {
    activeOperation.textContent = "未选择";
    activeOperationDetail.textContent = "请选择图片和处理方法";
    return;
  }

  activeOperation.textContent = operation.label;
  activeOperationDetail.textContent = `Type ${operation.type} / ${operation.description}`;
}

function clearResult() {
  revokeUrl(resultUrl);
  resultUrl = "";
  resultImage.removeAttribute("src");
  resultFrame.classList.remove("has-image");
  markActiveButton(null);
  setActiveOperation(null);
}

function resetAll() {
  selectedFile = null;
  imageInput.value = "";
  fileName.textContent = "未选择图片";
  fileType.textContent = "--";
  fileSize.textContent = "--";

  revokeUrl(originalUrl);
  originalUrl = "";
  originalImage.removeAttribute("src");
  originalFrame.classList.remove("has-image");

  clearResult();
  setStatus("请选择一张 PNG 或 JPG 图片。");
}

function handleFileChange() {
  const file = imageInput.files && imageInput.files[0];

  if (!file) {
    resetAll();
    return;
  }

  const allowedTypes = ["image/png", "image/jpeg"];
  if (!allowedTypes.includes(file.type)) {
    resetAll();
    setStatus("文件格式不支持。请上传 PNG、JPG 或 JPEG 图片。", "error");
    return;
  }

  selectedFile = file;
  fileName.textContent = file.name;
  fileType.textContent = formatFileType(file);
  fileSize.textContent = formatFileSize(file.size);

  revokeUrl(originalUrl);
  originalUrl = URL.createObjectURL(file);
  originalImage.src = originalUrl;
  originalFrame.classList.add("has-image");

  clearResult();
  setStatus("图片已载入。请选择一个处理方法发起 RPC 调用。", "success");
}

async function processImage(operation) {
  if (isProcessing) {
    return;
  }

  if (!selectedFile) {
    setStatus("请先选择图片，再选择处理方法。", "error");
    return;
  }

  isProcessing = true;
  setButtonsDisabled(true);
  markActiveButton(operation.type);
  setActiveOperation(operation);
  setStatus(`正在调用远程服务：${operation.label}...`, "busy");

  const formData = new FormData();
  formData.append("file", selectedFile);

  try {
    const response = await fetch(`${API_BASE_URL}/rpc/process?type=${operation.type}`, {
      method: "POST",
      body: formData
    });

    if (!response.ok) {
      throw new Error(buildHttpErrorMessage(response.status));
    }

    const contentType = response.headers.get("content-type") || "";
    if (!contentType.includes("image/")) {
      throw new Error("后端返回的不是图片数据，请检查 Controller 响应类型。");
    }

    const blob = await response.blob();
    if (!blob.size) {
      throw new Error("后端返回了空图片，请检查 C++ 图像处理节点。");
    }

    revokeUrl(resultUrl);
    resultUrl = URL.createObjectURL(blob);
    resultImage.src = resultUrl;
    resultFrame.classList.add("has-image");
    setStatus(`${operation.label} 处理完成，结果已返回。`, "success");
  } catch (error) {
    clearResult();
    markActiveButton(operation.type);
    setActiveOperation(operation);
    setStatus(normalizeErrorMessage(error), "error");
  } finally {
    isProcessing = false;
    setButtonsDisabled(false);
  }
}

function buildHttpErrorMessage(status) {
  if (status === 400) {
    return "请求参数无效，请确认图片和处理类型已正确提交。";
  }

  if (status === 404) {
    return "没有找到 /rpc/process 接口，请通过 Spring Boot 服务访问页面。";
  }

  if (status === 500) {
    return "后端处理失败。若 C++ 服务未启动，这是正常现象。";
  }

  return `请求失败，HTTP 状态码：${status}`;
}

function normalizeErrorMessage(error) {
  if (error instanceof TypeError) {
    return "无法连接 Spring Boot 后端，请确认服务已启动并通过 http://localhost:8081 访问。";
  }

  return error.message || "处理失败，请检查 Java 后端或 C++ 图像处理服务。";
}

function renderOperationButtons() {
  operations.forEach((operation) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "operation-button";
    button.dataset.type = String(operation.type);
    button.setAttribute("aria-pressed", "false");
    button.innerHTML = `
      <span class="button-main">
        <span class="button-code">${operation.code}</span>
        <span class="button-label">${operation.label}</span>
      </span>
      <span class="button-desc">${operation.description}</span>
    `;
    button.addEventListener("click", () => processImage(operation));
    operationButtons.appendChild(button);
  });
}

renderOperationButtons();
imageInput.addEventListener("change", handleFileChange);
clearButton.addEventListener("click", resetAll);
window.addEventListener("beforeunload", () => {
  revokeUrl(originalUrl);
  revokeUrl(resultUrl);
});
