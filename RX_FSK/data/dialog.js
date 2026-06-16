// Lightweight modal dialogs replacing native alert()/confirm(), styled via style.css.
//   showAlert(message)   -> Promise            (resolves when dismissed)
//   showConfirm(message) -> Promise<boolean>   (true = OK, false = Cancel)
// A single overlay element is built on first use and reused. Messages are inserted with
// textContent (XSS-safe; they may contain user-supplied names) and CSS white-space:pre-line
// keeps the "\n" line breaks the old alert()/confirm() text relied on.
(function (global) {
  var overlay, msgEl, okBtn, cancelBtn, current;

  function build() {
    overlay = document.createElement('div');
    overlay.className = 'modal-overlay';
    overlay.style.display = 'none';

    var box = document.createElement('div');
    box.className = 'modal-box';
    msgEl = document.createElement('div');
    msgEl.className = 'modal-msg';

    var actions = document.createElement('div');
    actions.className = 'modal-actions';
    cancelBtn = document.createElement('button');
    cancelBtn.type = 'button';
    cancelBtn.className = 'modal-btn modal-btn-cancel';
    cancelBtn.textContent = 'Cancel';
    okBtn = document.createElement('button');
    okBtn.type = 'button';
    okBtn.className = 'modal-btn modal-btn-primary';
    okBtn.textContent = 'OK';
    actions.appendChild(cancelBtn);
    actions.appendChild(okBtn);

    box.appendChild(msgEl);
    box.appendChild(actions);
    overlay.appendChild(box);
    document.body.appendChild(overlay);

    okBtn.addEventListener('click', function () { close(true); });
    cancelBtn.addEventListener('click', function () { close(false); });
    document.addEventListener('keydown', function (e) {
      if (!current) return;
      if (e.key === 'Enter') { e.preventDefault(); close(true); }
      else if (e.key === 'Escape') { e.preventDefault(); close(false); }
    });
  }

  function close(result) {
    if (!current) return;
    overlay.style.display = 'none';
    var resolve = current.resolve, isConfirm = current.isConfirm;
    current = null;
    resolve(isConfirm ? !!result : undefined);
  }

  function open(message, isConfirm) {
    if (!overlay) build();
    msgEl.textContent = message == null ? '' : String(message);
    cancelBtn.style.display = isConfirm ? '' : 'none';
    overlay.style.display = 'flex';
    return new Promise(function (resolve) {
      current = { resolve: resolve, isConfirm: isConfirm };
      okBtn.focus();
    });
  }

  global.showAlert = function (message) { return open(message, false); };
  global.showConfirm = function (message) { return open(message, true); };
})(window);
